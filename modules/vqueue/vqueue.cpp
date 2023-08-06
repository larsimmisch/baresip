/**
 * @file vqueue.cpp WAV Audio Source with priority queue,
 * interrupt & loop modes
 *
 * Copyright (C) 2023 Lars Immisch
 */
#include <stdio.h>
#include <assert.h>
#include <regex>
#include <string>
#include <vector>
#include <variant>
#include <re.h>
#include <re_atomic.h>
#include <rem.h>
#include <baresip.h>
#include "vqueue.h"


/**
 * @defgroup vqueue vqueue
 *
 * Audio module that implements a priority queue with loops and
 * interuption modes
 *
 * Sample config:
 *
 \verbatim
  vqueue_enqueue	<molecule>	discard|pause|mute|restart|
  								dont_interrupt|
								loop|m_dtmf_stop
  molecule:			(p file)|(r file <maxsilence>?)|
						(d <digits> <inter_digit_delay>?)
  vqueue_stop		<id>
  vqueue_cancel		<priority>
 \endverbatim
 */

const int max_priority = 5;

enum mode {
	m_discard,
	m_pause,
	m_mute,
	m_restart,
	m_dont_interrupt,
	m_loop,
	m_dtmf_stop,
};

struct Play {
	std::string filename;
	size_t size;
	size_t position;
};

struct Source {
	std::string filename;
	int max_silence;
};

struct DTMF {
	std::string dtmf;
	int inter_digit_delay;
};

using Atom = std::variant<Play, Source, DTMF>;

struct Molecule {
	std::vector<Atom> atoms;
	int current;
	int priority;
	int id;
	mode mode;
};

struct Position {
	size_t index;
	size_t offset;
};

struct vqueue {
	std::vector<Molecule> queue[max_priority];
	std::vector<Position> prev_positions;
	int current_id;
};

struct auplay_st {
	struct aufile *auf;
	struct auplay_prm prm;
	thrd_t thread;
	RE_ATOMIC bool run;
	void *sampv;
	size_t sampc;
	size_t num_bytes;
	auplay_write_h *wh;
	Play *play;
};

struct ausrc_st {
	struct tmr tmr;
	struct aufile *aufile;
	struct aubuf *aubuf;
	enum aufmt fmt;                 /**< Wav file sample format          */
	struct ausrc_prm prm;           /**< Audio src parameter             */
	uint32_t ptime;
	size_t sampc;
	RE_ATOMIC bool run;
	RE_ATOMIC bool started;
	thrd_t thread;
	ausrc_read_h *rh;
	ausrc_error_h *errh;
	Source* src;
};

static vqueue vqueue;
static struct ausrc *ausrc;
static struct auplay *auplay;


static int vqueue_thread(void *arg) {
	struct auplay_st *st = (struct auplay_st*)arg;
	uint64_t t;
	int dt;
	int err;
	uint32_t ptime = st->prm.ptime;

	t = tmr_jiffies();
	while (re_atomic_rlx(&st->run)) {
		struct auframe af;

		auframe_init(&af, (enum aufmt)st->prm.fmt, st->sampv, st->sampc,
			     st->prm.srate, st->prm.ch);

		af.timestamp = t * 1000;

		st->wh(&af, (void*)st->play->filename.c_str());

		err = aufile_write(st->auf, (const uint8_t*)st->sampv, st->num_bytes);
		if (err)
			break;

		t += ptime;
		dt = (int)(t - tmr_jiffies());
		if (dt <= 2)
			continue;

		sys_msleep(dt);
	}

	re_atomic_rlx_set(&st->run, false);

	return 0;
}

static int src_thread(void *arg) {
	uint64_t now, ts = tmr_jiffies();
	struct ausrc_st *st = (struct ausrc_st*)arg;
	int16_t *sampv;
	uint32_t ms = 4;

	re_atomic_rlx_set(&st->started, true);
	if (!st->ptime)
		ms = 0;

	sampv = (int16_t*)mem_alloc(st->sampc * sizeof(int16_t), NULL);
	if (!sampv)
		return ENOMEM;

	while (re_atomic_rlx(&st->run)) {
		struct auframe af;

		sys_msleep(ms);
		now = tmr_jiffies();
		if (ts > now)
			continue;

		auframe_init(&af, AUFMT_S16LE, sampv, st->sampc,
		             st->prm.srate, st->prm.ch);

		aubuf_read_auframe(st->aubuf, &af);

		st->rh(&af, (void*)st->src->filename.c_str());

		ts += st->ptime;

		if (aubuf_cur_size(st->aubuf) == 0)
			break;
	}

	mem_deref(sampv);
	re_atomic_rlx_set(&st->run, false);

	return 0;
}


extern "C" {

	static void vqueue_play_destructor(void *arg) {
		struct auplay_st *st = (struct auplay_st*)arg;
		/* Wait for termination of other thread */
		if (re_atomic_rlx(&st->run)) {
			debug("vqueue: stopping thread\n");
			re_atomic_rlx_set(&st->run, false);
			thrd_join(st->thread, NULL);
		}

		mem_deref(st->auf);
		mem_deref(st->sampv);
	}

	static void vqueue_src_destructor(void *arg) {
		struct ausrc_st *st = (struct ausrc_st*)arg;
		/* Wait for termination of other thread */
		if (re_atomic_rlx(&st->run)) {
			debug("vqueue: stopping recording thread\n");
			re_atomic_rlx_set(&st->run, false);
			thrd_join(st->thread, NULL);
		}
	}

	static int vqueue_player_alloc(struct auplay_st **stp,
		const struct auplay *ap, struct auplay_prm *prm,
		const char *dev, auplay_write_h *wh, void *arg)
	{
		struct auplay_st *st;
		int err = 0;

		if (!stp || !ap || !prm || !wh)
			return EINVAL;

		st = (struct auplay_st*)mem_zalloc(sizeof(*st), vqueue_play_destructor);
		if (!st)
			return ENOMEM;

		st->prm.srate = prm->srate;
		st->prm.ch	= prm->ch;
		st->prm.ptime = prm->ptime;
		st->prm.fmt   = prm->fmt;

		st->wh = wh;

		if (err)
			mem_deref(st);
		else
			*stp = st;

		info ("vqueue: opening player (%s, %u Hz, %d channels, device %s, "
			"ptime %u)\n", st->play->filename.c_str(), prm->srate, prm->ch, dev, prm->ptime);
		re_atomic_rlx_set(&st->run, true);
		err = thread_create_name(&st->thread, "vqueue_play", vqueue_thread, st);
		if (err) {
			re_atomic_rlx_set(&st->run, false);
		}

		return err;
	}

	static int vqueue_src_alloc(struct ausrc_st **stp,
		const struct ausrc *ap,	struct ausrc_prm *prm,
		const char *dev, ausrc_read_h *rh, ausrc_error_h *errh, void *arg)
	{
		struct ausrc_st *st;
		int err = 0;

		if (!stp || !ap || !prm || !rh)
			return EINVAL;

		st = (struct ausrc_st*)mem_zalloc(sizeof(*st), vqueue_src_destructor);
		if (!st)
			return ENOMEM;

		st->prm.srate = prm->srate;
		st->prm.ch	= prm->ch;
		st->prm.ptime = prm->ptime;
		st->prm.fmt   = prm->fmt;

		st->rh  = rh;
		st->src = (Source*)arg;

		info ("vqueue: opening source (%s, %u Hz, %d channels, device %s, "
			"ptime %u)\n", st->src->filename.c_str(), prm->srate, prm->ch, dev, prm->ptime);

		if (err)
			mem_deref(st);
		else
			*stp = st;

		return err;
	}

	static int module_init(void)
	{
		int err = auplay_register(&auplay, baresip_auplayl(),
				   "vqueue", vqueue_player_alloc);
		err |= ausrc_register(&ausrc, baresip_ausrcl(),
				  "vqueue", vqueue_src_alloc);

		if (err) {
			return err;
		}
	}

	static int module_close(void)
	{
		auplay = (struct auplay*)mem_deref((void*)auplay);
		ausrc  = (struct ausrc*)mem_deref((void*)ausrc);

		return 0;
	}

	EXPORT_SYM const struct mod_export DECL_EXPORTS(vqueue) = {
		"vqueue_enqueue",
		"vqueue_stop"
		"vqueue_cancel",
		module_init,
		module_close
	};
};

bool is_atom_start(const std::string &token) {
	if (token == "p" || token == "r" || token == "d") {
		return true;
	}

	return false;
}

int vqueue_enqueue(const char* args)
{
	std::string sargs(args);
	std::regex ws("\\s+");

	std::sregex_token_iterator iter(sargs.begin(), sargs.end(), ws, -1);
	std::sregex_token_iterator end;

	std::vector<std::string> vec(iter, end);
	std::vector<std::string> tokens;

	for (auto a: vec)
	{
		std::smatch m;
		if (!std::regex_match(a, m, ws)) {
			tokens.push_back(a);
		}
	}

	Molecule m;

	auto token = tokens.begin();
	if (token == tokens.end()) {
		perror("missing mode");
		return 0;
	}

	if (*token == "loop") {
		m.mode = m_loop;
	}
	else if (*token == "mute") {
		m.mode = m_mute;
	}
	else if (*token == "discard") {
		m.mode = m_discard;
	}
	else if (*token == "pause") {
		m.mode = m_pause;
	}
	else if (*token == "restart") {
		m.mode = m_restart;
	}
	else if (*token == "dont_interrupt") {
		m.mode = m_dont_interrupt;
	}
	else if (*token == "loop") {
		m.mode = m_loop;
	}
	else if (*token == "dtmf_stop") {
		m.mode = m_dtmf_stop;
	}
	++token;

	if (token == tokens.end()) {
		perror("missing priority");
		return 0;
	}

	m.priority = std::stol(*token);

	while (token != tokens.end()) {
		Atom atom;

		if (*token == "p") {
			auto args = token;
			++args;

			if (args != tokens.end()) {
				Play play;
				play.filename = *args;

				++args;
				if (args != tokens.end()) {

					if (!is_atom_start(*args)) {
						play.position = std::stol(*args);
					}
				}

				m.atoms.push_back(play);
			}
			else {
				perror("No filename after play atom");
				return 0;
			}
		}
		else if (*token == "r") {
			auto args = token;
			if (args != tokens.end()) {
				Source src;
				src.filename = *args;

				++args;
				if (args != tokens.end()) {

					if (!is_atom_start(*args)) {
						src.max_silence = std::stol(*args);
					}
				}

				m.atoms.push_back(src);
			}
			else {
				perror("No filename after record atom");
				return 0;
			}
		}
		else if (*token == "d") {
			auto args = token;
			if (args != tokens.end()) {
				DTMF dtmf;
				dtmf.dtmf = *args;

				++args;
				if (args != tokens.end()) {

					if (!is_atom_start(*args)) {
						dtmf.inter_digit_delay = std::stol(*args);
					}
				}

				m.atoms.push_back(dtmf);
			}
			else {
				perror("No digits after atom_dtmf");
				return 0;
			}
		}
		m.atoms.push_back(atom);
	}

	if (m.atoms.size() == 0) {
		perror("No atom in molecule");
		return 0;
	}

	return vqueue.current_id++;
}

void vqueue_stop(const char* args) {
}

void vqueue_cancel(const char* args) {
}

