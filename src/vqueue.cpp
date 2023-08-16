/**
 * @file src/play.c  Audio-file player
 *
 * Copyright (C) 2010 Alfred E. Heggestad
 */
#include <stdlib.h>
#include <string.h>
#include <re.h>
#include <rem.h>
#include <baresip.h>
#include <string>
#include <regex>
#include "core.h"
#include "../modules/aufile/aufile.h"

enum { PTIME = 40 };

const int max_priority = 5;

enum mode {
	m_first = 1,
	m_discard = 1,
	m_pause = 2,
	m_mute = 4,
	m_restart = 8,
	m_dont_interrupt = 16,
	m_loop = 32,
	m_dtmf_stop = 64,
	m_last = 64,
};

struct Play {
	std::string filename;
	size_t size = 0;
	size_t offset = 0;
};

struct Record {
	std::string filename;
	int max_silence = 500;
};

struct DTMF {
	std::string dtmf;
	int inter_digit_delay;
	size_t pos = 0;
};

using Atom = std::variant<Play, Record, DTMF>;

struct Molecule {
	std::vector<Atom> atoms;
	size_t time_stopped = 0;
	int current;
	int priority;
	int id;
	mode mode;
};

struct VQueue {
	std::vector<Molecule> molecules[max_priority];
	int current_id;
};

static ausrc_st* g_rec;
static struct play *g_play;

static VQueue vqueue;

bool is_atom_start(const std::string &token) {
	if (token[0] == 'p' || token[0] == 'r' || token[0] == 'd') {
		return true;
	}

	return false;
}

const char* mode_string(mode m) {

	std::string modestr;

	for (int e = m_first; e < m_last; e <<= 1) {
		switch (e) {

			if (modestr.size()) {
				modestr += "|";
			}

			case m_discard:
				modestr += "discard";
				break;
			case m_pause:
				modestr += "pause";
				break;
			case m_mute:
				modestr += "mute";
				break;
			case m_restart:
				modestr += "restart";
				break;
			case m_dont_interrupt:
				modestr += "dont_interrupt";
				break;
			case m_loop:
				modestr += "loop";
				break;
			case m_dtmf_stop:
				modestr += "dtmf_stop";
				break;

			printf(modestr.c_str());
		}
	}

	return std::move(modestr.c_str());
}

void src_handler(struct auframe *af, void *arg) {}

void src_error_handler(int err, const char *str, void *arg) {}

int enqueue(const Molecule& m) {

	struct config *cfg = conf_config();

	size_t time_now = tmr_jiffies();

	for (int p = max_priority; p >= 0; --p) {
		if (vqueue.molecules[p].size()) {
			vqueue.molecules[p].back().time_stopped = time_now;
		}
	}

	/* Stop the current player or recorder, if any */
	g_play = (struct play*)mem_deref(g_play);
	g_rec = (struct ausrc_st*)mem_deref(g_rec);

	vqueue.molecules[m.priority].push_back(m);

	while (true) {

		for (int p = max_priority; p >= 0; --p) {

			while (vqueue.molecules[p].size()) {

				for (auto m : vqueue.molecules[p]) {

					for (auto a: m.atoms) {

						if (std::holds_alternative<Play>(a)) {

							const Play& play = std::get<Play>(a);

							int err = play_file(&g_play, baresip_player(), play.filename.c_str(), 0,
									cfg->audio.alert_mod, cfg->audio.alert_dev, play.offset);
							if (err) {
								return err;
							}
						}
						else if (std::holds_alternative<DTMF>(a)) {

							DTMF& d = std::get<DTMF>(a);

							++d.pos;
							if (d.pos > d.dtmf.size()) {
								d.pos = 0;
								break;
							}

							std:: string filename = "sound";
							if (d.dtmf[d.pos] == '*') {
								filename += "star.wav";
							}
							else if (d.dtmf[d.pos] == '#') {
								filename += "route.wav";
							}
							else {
								filename.append((char)tolower(d.dtmf[d.pos]), 1);
								filename += ".wav";
							}
							int err = play_file(&g_play, baresip_player(), filename.c_str(), 0,
									cfg->audio.alert_mod, cfg->audio.alert_dev, 0);
							if (err) {
								return err;
							}
						}
						else if (std::holds_alternative<Record>(a)) {

							uint32_t srate = 0;
							uint32_t channels = 0;

							conf_get_u32(conf_cur(), "file_srate", &srate);
							conf_get_u32(conf_cur(), "file_channels", &channels);

							if (!srate) {
								srate = 16000;
							}

							if (!channels) {
								channels = 1;
							}

							ausrc_prm sprm;

							sprm.ch = channels;
							sprm.srate = srate;
							sprm.ptime = PTIME;
							sprm.fmt = AUFMT_S16LE;

							const struct ausrc *ausrc = ausrc_find(baresip_ausrcl(), "aufile");

							int err = ausrc->alloch(&g_rec, ausrc,
								&sprm, nullptr, nullptr, nullptr, nullptr);

							if (err) {
								return err;
							}
						}
					}
				}
			}

			auto begin = vqueue.molecules[p].begin();
			if (begin->mode != m_loop) {
				vqueue.molecules[p].erase(begin);
			}
		}
	}

	return 0;
}

int enqueue(const char* args) {

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
		perror("missing priority");
		return EINVAL;
	}

	try {
		m.priority = std::stol(*token);
	}
	catch(std::exception&) {
		perror("invalid priority");
		return EINVAL;
	}

	m.mode = (mode)0;

	for (int i = 0; i < 2; ++i) {

		if (*token == "loop") {
			m.mode = (mode)(m.mode | m_loop);
		}
		else if (*token == "mute") {
			m.mode = (mode)(m.mode | m_mute);
		}
		else if (*token == "discard") {
			m.mode = (mode)(m.mode | m_discard);
		}
		else if (*token == "pause") {
			m.mode = (mode)(m.mode | m_pause);
		}
		else if (*token == "restart") {
			m.mode = (mode)(m.mode | m_restart);
		}
		else if (*token == "dont_interrupt") {
			m.mode = (mode)(m.mode | m_dont_interrupt);
		}
		else if (*token == "dtmf_stop") {
			m.mode = (mode)(m.mode | m_dtmf_stop);
		}
		else {
			break;
		}
		++token;

		if (token == tokens.end()) {
			perror("missing mode");
			return 0;
		}
	}

	// info("adding Molecule priority: %d, mode: %s", m.priority, mode_string(m.mode));

	while (token != tokens.end()) {
		Atom atom;

		if (*token == "p" || *token == "play") {
			auto args = token;
			++args;

			if (args != tokens.end()) {
				Play play;
				play.filename = *args;

				++args;
				if (args != tokens.end()) {

					if (!is_atom_start(*args)) {
						play.offset = std::stol(*args);
					}
					else {
						play.offset = 0;
					}
				}

				info("\tplay %s %d", play.filename.c_str(), play.offset);
				m.atoms.push_back(play);
			}
			else {
				perror("No filename after play atom");
				return 0;
			}
		}
		else if (*token == "r" || *token == "record") {
			auto args = token;
			if (args != tokens.end()) {
				Record record;
				record.filename = *args;

				++args;
				if (args != tokens.end()) {

					if (!is_atom_start(*args)) {
						record.max_silence = std::stol(*args);
					}
					else {
						record.max_silence = 500;
					}
				}

				info("\trecord %s %d", record.filename.c_str(), record.max_silence);
				m.atoms.push_back(record);
			}
			else {
				perror("No filename after record atom");
				return 0;
			}
		}
		else if (*token == "d" || *token == "dtmf") {
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

	return enqueue(m);
}
