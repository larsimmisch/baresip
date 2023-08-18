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
	m_discard = 1,
	m_pause = 2,
	m_mute = 4,
	m_restart = 8,
	m_dont_interrupt = 16,
	m_loop = 32,
	m_dtmf_stop = 64,
	m_last = 64,
};

class Play {

public:

	Play() {}
	Play(const std::string& filename) { set_filename(filename); };

	// returns the size in ms or 0 in case of an error
	size_t set_filename(const std::string& filename);
	const std::string& filename() const { return _filename; }

	void set_offset(size_t offset) { _offset = offset; }
	size_t offset() const { return _offset; }

	size_t length() const { return _length; }

protected:

	std::string _filename;
	size_t _length = 0; // length in ms
	size_t _offset = 0; // offset in ms
};

class Record {

public:

	Record() {}
	Record(const std::string& filename) : _filename(filename) {}

	void set_filename(const std::string& filename) { _filename = filename; }
	const std::string& filename() { return _filename; }

	void set_max_silence(int max_silence) { _max_silence = max_silence; }
	size_t max_silence() const { return _max_silence; }

	size_t length() const { return _length; }

protected:

	std::string _filename;
	int _max_silence = 1000;
	size_t _length = 0;
};

class DTMF {

public:

	DTMF() {}
	DTMF(const std::string& dtmf) : _dtmf(dtmf) {}

	char current() const { return _dtmf[_pos]; }
	size_t operator++() { return  ++_pos; }
	size_t size() const { return _dtmf.size(); }
	void reset() { _pos = 0; }

	void set_dtmf(const std::string& dtmf) { _dtmf = dtmf; }
	const std::string& dtmf() const { return _dtmf; }

	int inter_digit_delay() const { return _inter_digit_delay; }
	void set_inter_digit_delay(int inter_digit_delay) { _inter_digit_delay = inter_digit_delay; }

	size_t length() const { return _length; }

protected:

	std::string _dtmf;
	int _inter_digit_delay = 100;
	size_t _pos = 0;
	size_t _length = 0;
};

using Atom = std::variant<Play, Record, DTMF>;

struct Molecule {
	std::vector<Atom> atoms;
	size_t time_stopped = 0;
	size_t length_ms = 0;
	size_t current = 0;
	int priority = 0;
	mode mode;

	size_t length() const;
};

struct VQueue {
	std::vector<Molecule> molecules[max_priority];
	int current_id;
};

size_t Molecule::length() const {
	size_t l = 0;

	for (auto a: atoms) {

		if (std::holds_alternative<Play>(a)) {
			l += std::get<Play>(a).length();
		}
		else if (std::holds_alternative<DTMF>(a)) {
			l += std::get<DTMF>(a).length();
		}
		if (std::holds_alternative<Record>(a)) {
			l += std::get<Record>(a).length();
		}
	}

	return l;
}

void play_stop_handler(struct play *play, void *arg);

static ausrc_st* g_rec;
static struct play *g_play;

static VQueue vqueue;

bool is_atom_start(const std::string &token) {
	if (token[0] == 'p' || token[0] == 'r' || token[0] == 'd') {
		return true;
	}

	return false;
}

size_t Play::set_filename(const std::string& filename) {

	struct aufile* au;
	struct aufile_prm prm;

	int err = aufile_open(&au, &prm, _filename.c_str(), AUFILE_READ);
	if (err) {
		return 0;
	}

	size_t length = aufile_get_length(au, &prm);

	mem_deref(au);

	_filename = filename;

	return length;
}

std::string mode_string(mode m) {

	std::string modestr;

	for (int e = m_last; e != 0; e >>= 1) {

		if (modestr.size() && modestr.back() != '|') {
			modestr += "|";
		}

		mode em = (mode)(m & e);

		switch (em) {
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
		}
	}

	if (modestr.size() && modestr.back() == '|') {
		modestr.pop_back();
	}

	return modestr;
}

void src_handler(struct auframe *af, void *arg) {}

void src_error_handler(int err, const char *str, void *arg) {}

int schedule(void* arg) {

	struct config *cfg = conf_config();
	struct audio* audio = call_audio((struct call*)arg);

	for (int p = max_priority; p >= 0; --p) {

		for (auto m : vqueue.molecules[p]) {

			if (m.atoms.size()) {

				if (m.current >= m.atoms.size()) {
					warning("Current Atom %d is out of bounds, playing last instead (%d)\n",
						m.current, m.atoms.size() - 1);
					m.current = m.atoms.size() - 1;
				}
				Atom a = vqueue.molecules[p].back().atoms[m.current];

				if (std::holds_alternative<Play>(a)) {

					const Play& play = std::get<Play>(a);

					info("playing %s\n", play.filename().c_str());

					// mute
					// audio_mute(audio, true);

					int err = play_file_ext(&g_play, baresip_player(), play.filename().c_str(), 0,
						cfg->audio.alert_mod, cfg->audio.alert_dev,
						play.offset());
					if (err) {
						return err;
					}
					play_set_finish_handler(g_play, play_stop_handler, arg);
				}
				else if (std::holds_alternative<DTMF>(a)) {

					DTMF& d = std::get<DTMF>(a);

					if (++d > d.size()) {
						d.reset();
						break;
					}

					std:: string filename = "sound";
					if (d.current() == '*') {
						filename += "star.wav";
					}
					else if (d.current() == '#') {
						filename += "route.wav";
					}
					else {
						filename.append((char)tolower(d.current()), 1);
						filename += ".wav";
					}

					info("DTMF playing %s\n", filename.c_str());

					// mute
					// audio_mute(audio, true);

					int err = play_file_ext(&g_play, baresip_player(), filename.c_str(), 0,
							cfg->audio.alert_mod, cfg->audio.alert_dev, 0);
					if (err) {
						return err;
					}
					play_set_finish_handler(g_play, play_stop_handler, arg);
				}
				else if (std::holds_alternative<Record>(a)) {

					// unmute
					// audio_mute(audio, false);

					Record& record = std::get<Record>(a);

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

					info("recording %s\n", record.filename().c_str());

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

	return 0;
}

void play_stop_handler(struct play *play, void *arg) {

	info("play file stopped\n");

	/* Stop the current player or recorder, if any */
	g_play = (struct play*)mem_deref(g_play);
	g_rec = (struct ausrc_st*)mem_deref(g_rec);

	for (int p = max_priority; p >= 0; --p) {
		if (vqueue.molecules[p].size()) {
			Molecule &m = vqueue.molecules[p].back();

			if (m.mode & m_loop) {
				m.current = (m.current + 1) % m.atoms.size();
				break;
			}
			if (m.current >= m.atoms.size()) {
				// the molecule is completed
				vqueue.molecules[p].pop_back();
			}
			else {
				++m.current;
			}
		}
	}
	schedule(arg);
}

int enqueue(const Molecule& m, void* arg) {

	if (m.priority > 0) {
		for (int p = m.priority - 1; p >= 0; --p) {
			if (vqueue.molecules[p].size()) {
				vqueue.molecules[p].back().time_stopped = tmr_jiffies();
				break;
			}
		}
	}

	vqueue.molecules[m.priority].push_back(m);

	/* Stop the current player or recorder, if any */
	g_play = (struct play*)mem_deref(g_play);
	g_rec = (struct ausrc_st*)mem_deref(g_rec);

	return schedule(arg);
}

int enqueue(const char* mdesc, void* arg) {

	std::string smdesc(mdesc);
	std::regex ws("\\s+");

	std::sregex_token_iterator iter(smdesc.begin(), smdesc.end(), ws, -1);
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

	++token;

	if (token == tokens.end()) {
		perror("missing mode");
		return 0;
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

	info("adding Molecule priority: %d, mode: %s\n", m.priority, mode_string(m.mode).c_str());

	while (token != tokens.end()) {

		if (*token == "p" || *token == "play") {

			++token;
			if (token != tokens.end()) {
				Play play;
				play.set_filename(*token);

				++token;
				if (token != tokens.end()) {

					if (!is_atom_start(*token)) {
						play.set_offset(std::stol(*token));
					}
					else {
						++token;
					}
				}

				m.atoms.push_back(play);
			}
			else {
				perror("No filename after play atom");
				return 0;
			}
		}
		else if (*token == "r" || *token == "record") {
			++token;
			if (token != tokens.end()) {
				Record record;
				record.set_filename(*token);

				++token;
				if (token != tokens.end()) {

					if (!is_atom_start(*token)) {
						record.set_max_silence(std::stol(*token));
						++token;

					}
				}

				m.atoms.push_back(record);
			}
			else {
				perror("No filename after record atom");
				return 0;
			}
		}
		else if (*token == "d" || *token == "dtmf") {
			++token;
			if (token != tokens.end()) {
				DTMF dtmf;
				dtmf.set_dtmf(*token);
				++token;

				if (!is_atom_start(*token)) {
					dtmf.set_inter_digit_delay(std::stol(*token));
					++token;
				}

				m.atoms.push_back(dtmf);
			}
			else {
				perror("No digits after atom_dtmf");
				return 0;
			}
		}
	}

	if (m.atoms.size() == 0) {
		perror("No atom in molecule");
		return 0;
	}

	return enqueue(m, arg);
}
