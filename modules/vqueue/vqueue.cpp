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

struct Source {
	std::string filename;
	size_t size;
	size_t position;
};

struct Sink {
	std::string filename;
	int max_silence;
};

struct DTMF {
	std::string dtmf;
	int inter_digit_delay;
};

using Atom = std::variant<Source, Sink, DTMF>;

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
	std::vector<Molecule> molecules[max_priority];
	std::vector<Position> prev_positions;
	int current_id;
};

struct vq_st {
	vqueue queue;
	struct tmr tmr;
	struct aufile *auf;
	struct aubuf *aubuf;
	struct auplay_prm play_prm;
	struct ausrc_prm src_prm;       /**< Audio src parameter             */
	enum aufmt fmt;                 /**< Wav file sample format          */
	thrd_t thread;
	RE_ATOMIC bool run;
	void *sampv;
	size_t sampc;
	size_t num_bytes;
	uint32_t ptime;
	ausrc_read_h *rh;
	ausrc_error_h *errh;
	auplay_write_h *wh;
	void *arg;
};

struct ausrc *ausrc;
struct auplay *auplay;
static std::vector<vq_st> channels;

/**
* Process a single atom
*
* returns the time expired in ms.
*/
static size_t process(const Atom& current, vq_st* vq) {

	if (std::holds_alternative<Sink>(current)) {

		const Sink& sink = std::get<Sink>(current);
		uint32_t ptime = vq->play_prm.ptime;
		uint64_t t = tmr_jiffies();

		while (re_atomic_rlx(&vq->run)) {
			struct auframe af;

			auframe_init(&af, (enum aufmt)vq->play_prm.fmt,
				vq->sampv, vq->sampc,
				vq->play_prm.srate, vq->play_prm.ch);

			af.timestamp = t * 1000;

			vq->wh(&af, (void*)sink.filename.c_str());

			int err = aufile_write(vq->auf,
				(const uint8_t*)vq->sampv, vq->num_bytes);
			if (err)
				break;

			t += ptime;
			int dt = (int)(t - tmr_jiffies());
			if (dt <= 2)
				continue;

			sys_msleep(dt);
		}
	}
	else if (std::holds_alternative<Source>(current)) {

		const Source& src = std::get<Source>(current);
		uint64_t now, ts = tmr_jiffies();
		uint32_t ms = 4;

		if (!vq->ptime)
			ms = 0;

		int16_t *sampv = (int16_t*)mem_alloc(vq->sampc * sizeof(int16_t), NULL);
		if (!sampv)
			return ENOMEM;

		while (re_atomic_rlx(&vq->run)) {
			struct auframe af;

			sys_msleep(ms);
			now = tmr_jiffies();
			if (ts > now)
				continue;

			auframe_init(&af, AUFMT_S16LE, sampv, vq->sampc,
				vq->src_prm.srate, vq->src_prm.ch);

			aubuf_read_auframe(vq->aubuf, &af);

			vq->rh(&af, (void*)src.filename.c_str());

			ts += vq->ptime;

			if (aubuf_cur_size(vq->aubuf) == 0)
				break;
		}

		mem_deref(sampv);
	}
	else if (std::holds_alternative<DTMF>(current)) {

	}
}

static int vqueue_thread(void *arg) {

	vq_st* vq = (vq_st*)arg;

	while (re_atomic_rlx(&vq->run)) {

		for (int p = max_priority; p >= 0; --p) {
			if (vq->queue.molecules[p].size()) {
				const Atom &current = vq->queue.molecules[p].front().atoms.front();

				size_t time_expired = process(current, vq);
			}
		}
	}

	return 0;
}

bool is_atom_start(const std::string &token) {
	if (token == "p" || token == "r" || token == "d") {
		return true;
	}

	return false;
}

const char* mode_string(mode m) {
	switch (m) {
		case m_discard:
			return "discard";
		case m_pause:
			return "pause";
		case m_mute:
			return "mute";
		case m_restart:
			return "restart";
		case m_dont_interrupt:
			return "dont_interrupt";
		case m_loop:
			return "loop";
		case m_dtmf_stop:
			return "dtmf_stop";
	}
}

extern "C" {

	static void timeout(void *arg)
	{
		struct vq_st *vq = (vq_st*)arg;
		tmr_start(&vq->tmr, vq->ptime ? vq->ptime : 40, timeout, vq);

		/* check if audio buffer is empty */
		if (!re_atomic_rlx(&vq->run)) {
			tmr_cancel(&vq->tmr);

			info("vqueue: end of file\n");

			/* error handler must be called from re_main thread */
			if (vq->errh)
				vq->errh(0, "end of file", vq->arg);
		}
	}

	static int read_file(vq_st *vq)
	{
		struct mbuf *mb = NULL;
		int err;
		size_t n;
		struct mbuf *mb2 = NULL;
		struct auframe af;

		auframe_init(&af, vq->fmt, NULL, 0, vq->src_prm.srate, vq->src_prm.ch);

		for (;;) {
			uint16_t *sampv;
			uint8_t *p;
			size_t i;

			mem_deref(mb);
			mb = mbuf_alloc(4096);
			if (!mb)
				return ENOMEM;

			mb->end = mb->size;

			err = aufile_read(vq->auf, mb->buf, &mb->end);
			if (err)
				break;

			if (mb->end == 0) {
				info("aufile: read end of file\n");
				break;
			}

			/* convert from Little-Endian to Native-Endian */
			n = mb->end;
			sampv = (uint16_t *)mb->buf;
			p     = (uint8_t *)mb->buf;

			switch (vq->fmt) {
			case AUFMT_S16LE:
				/* convert from Little-Endian to Native-Endian */
				for (i = 0; i < n/2; i++)
					sampv[i] = sys_ltohs(sampv[i]);

				aubuf_append_auframe(vq->aubuf, mb, &af);
				break;
			case AUFMT_PCMA:
			case AUFMT_PCMU:
				mb2 = mbuf_alloc(2 * n);
				for (i = 0; i < n; i++) {
					err |= mbuf_write_u16(mb2,
						vq->fmt == AUFMT_PCMA ?
						(uint16_t) g711_alaw2pcm(p[i]) :
						(uint16_t) g711_ulaw2pcm(p[i]) );
				}

				mbuf_set_pos(mb2, 0);
				aubuf_append_auframe(vq->aubuf, mb2, &af);
				mem_deref(mb2);
				break;

			default:
				err = ENOSYS;
				break;
			}

			if (err)
				break;
		}

		info("vqueue: loaded %zu bytes\n", aubuf_cur_size(vq->aubuf));
		mem_deref(mb);
		return err;
	}

	void vqueue_destructor(void *arg) {
		vq_st* vq = (vq_st*)arg;

		/* Wait for termination of other thread */
		if (re_atomic_rlx(&vq->run)) {
			debug("vqueue: stopping thread\n");
			re_atomic_rlx_set(&vq->run, false);
			thrd_join(vq->thread, NULL);
		}

		mem_deref(vq->auf);
		mem_deref(vq->sampv);
	}

	int vqueue_src_alloc(struct ausrc_st **stp, const struct ausrc *as,
		     struct ausrc_prm *prm, const char *dev,
		     ausrc_read_h *rh, ausrc_error_h *errh, void *arg)
	{
		int err = 0;

		if (!stp || !as || !prm || !rh) {
			return EINVAL;
		}

		vq_st* vq = NULL;

		for (auto ch: channels) {
			if (ch.arg == arg) {
				info("vqueue: found existing channel");
				vq = &ch;
				break;
			}
		}
		vq->play_prm.srate = prm->srate;
		vq->play_prm.ch	= prm->ch;
		vq->play_prm.ptime = prm->ptime;
		vq->play_prm.fmt   = prm->fmt;

		vq->rh = rh;

		if (!err) {
			*stp = (struct ausrc_st*)vq;
		}

		info ("vqueue: opening player (%u Hz, %d channels, device %s, "
			"ptime %u, arg %p)\n", prm->srate, prm->ch, dev, prm->ptime, arg);

	}

	int vqueue_play_alloc(struct auplay_st **stp,
		const struct auplay *ap, struct auplay_prm *prm,
		const char *dev, auplay_write_h *wh, void *arg)
	{
		int err = 0;

		if (!stp || !ap || !prm || !wh) {
			return EINVAL;
		}

		vq_st* vq = NULL;

		for (auto ch: channels) {
			if (ch.arg == arg) {
				info("vqueue: found existing channel");
				vq = &ch;
				break;
			}
		}

		vq->play_prm.srate = prm->srate;
		vq->play_prm.ch	= prm->ch;
		vq->play_prm.ptime = prm->ptime;
		vq->play_prm.fmt   = prm->fmt;

		vq->wh = wh;

		if (!err) {
			*stp = (struct auplay_st*)vq;
		}

		info ("vqueue: opening player (%u Hz, %d channels, device %s, "
			"ptime %u, arg %p)\n", prm->srate, prm->ch, dev, prm->ptime, arg);

		return err;
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

		info("adding Molecule priority: %d, mode: %s", m.priority, mode_string(m.mode));

		while (token != tokens.end()) {
			Atom atom;

			if (*token == "p") {
				auto args = token;
				++args;

				if (args != tokens.end()) {
					Source src;
					src.filename = *args;

					++args;
					if (args != tokens.end()) {

						if (!is_atom_start(*args)) {
							src.position = std::stol(*args);
						}
						else {
							src.position = 0;
						}
					}

					info("\tsrc %s %d", src.filename.c_str(), src.position);
					m.atoms.push_back(src);
				}
				else {
					perror("No filename after play atom");
					return 0;
				}
			}
			else if (*token == "r") {
				auto args = token;
				if (args != tokens.end()) {
					Sink sink;
					sink.filename = *args;

					++args;
					if (args != tokens.end()) {

						if (!is_atom_start(*args)) {
							sink.max_silence = std::stol(*args);
						}
						else {
							sink.max_silence = 500;
						}
					}

					info("\tsink %s %d", sink.filename.c_str(), sink.max_silence);
					m.atoms.push_back(sink);
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
						else {
							dtmf.inter_digit_delay = 40;
						}
					}

					info("\tdtmf %s %d", dtmf.dtmf.c_str(), dtmf.inter_digit_delay);
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

		return 17;
	}

	void vqueue_stop(const char* args) {
	}

	void vqueue_cancel(const char* args) {
	}
};
