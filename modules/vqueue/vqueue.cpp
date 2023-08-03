/**
 * @file vqueue.cpp WAV Audio Source with priority queue, interrupt & loop modes
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
 * Audio module that implements a priority queue with loops and interuption modes
 *
 * Sample config:
 *
 \verbatim
  vqueue_enqueue	<molecule> 	discard|pause|mute|restart|dont_interrupt|loop|m_dtmf_stop
  molecule:			(p file)|(r file <maxsilence>?)|(d <digits> <inter_digit_delay>?)
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

struct Record {
	std::string filename;
	int max_silence;
};

struct DTMF {
	std::string dtmf;
	int inter_digit_delay;
};

using Atom = std::variant<Play, Record, DTMF>;

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
	thrd_t thread;
	RE_ATOMIC bool run;
	struct auplay_prm prm;
	char* device;
};

struct ausrc_st {
	thrd_t thread;
	RE_ATOMIC bool run;
	struct ausrc_prm prm;
	char* device;
};

static struct vqueue *vqueue;
static struct ausrc *ausrc;
static struct auplay *auplay;

extern "C" {

	static int vqueue_player_alloc(struct auplay_st **stp, const struct auplay *ap,
		struct auplay_prm *prm, const char *dev, auplay_write_h *wh, void *arg)
	{
		struct auplay_st *st;
		int err = 0;

		if (!stp || !ap || !prm || !wh)
			return EINVAL;

		info ("vqueue: opening player (%u Hz, %d channels, device %s, "
			"ptime %u)\n", prm->srate, prm->ch, dev, prm->ptime);

		st = mem_zalloc(sizeof(*st), auplay_destructor);
		if (!st)
			return ENOMEM;

		err = str_dup(&st->device, device);
		if (err)
			goto out;

		st->play_prm.srate = prm->srate;
		st->play_prm.ch    = prm->ch;
		st->play_prm.ptime = prm->ptime;
		st->play_prm.fmt   = prm->fmt;

		st->sampsz = aufmt_sample_size(prm->fmt);

		st->wh  = wh;
		st->arg = arg;

	out:
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
		auplay = mem_deref((void*)auplay);
		ausrc  = mem_deref((void*)ausrc);

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

size_t vqueue_enqueue(const char* args)
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
		error("missing mode");
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
		error("missing priority");
		return 0;
	}

	m.priority = std::stol(*token);

	while (token != tokens.end()) {
		Atom atom;

		if (*token == "p") {
			auto args = token;
			args++;

			if (args != tokens.end()) {
				Play play;
				play.filename = *args;

				args++;
				if (args != tokens.end()) {

					if (!is_atom_start(*args)) {
						play.position = std::stol(*args);
					}
				}

				m.atoms.push_back(play);
			}
			else {
				error("No filename after play atom");
				return 0;
			}
		}
		else if (*token == "r") {
			auto args = token;
			if (args != tokens.end()) {
				Record record;
				record.filename = *args;

				args++;
				if (args != tokens.end()) {

					if (!is_atom_start(*args)) {
						record.max_silence = std::stol(*args);
					}
				}

				m.atoms.push_back(record);
			}
			else {
				error("No filename after record atom");
				return 0;
			}
		}
		else if (*token == "d") {
			auto args = token;
			if (args != tokens.end()) {
				DTMF dtmf;
				dtmf.dtmf = *args;

				args++;
				if (args != tokens.end()) {

					if (!is_atom_start(*args)) {
						dtmf.inter_digit_delay = std::stol(*args);
					}
				}

				m.atoms.push_back(dtmf);
			}
			else {
				error("No digits after atom_dtmf");
				return 0;
			}
		}
		m.atoms.push_back(atom);
	}

	if (m.atoms.size() == 0) {
		error("No atom in molecule");
		return 0;
	}

	assert(vqueue);

	return vqueue->current_id++;
}

void vqueue_stop(const char* args)
{

}

void vqueue_cancel(const char* args)
{

}

