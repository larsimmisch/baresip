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
	m_last = 32,
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
	const std::string& filename() const { return _filename; }

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

	void set_offset(size_t offset) {}

	size_t length() const { return _length; }

protected:

	std::string _dtmf;
	std::string _lengths;
	int _inter_digit_delay = 100;
	size_t _pos = 0;
	size_t _length = 0;
};

using Atom = std::variant<Play, Record, DTMF>;

struct Molecule {
	std::vector<Atom> atoms;
	size_t time_started = 0;
	size_t time_stopped = 0;
	size_t position = 0;
	size_t current = 0;
	int priority = 0;
	mode mode;

	size_t length(int start = 0, int end = -1) const;
	void set_position(size_t position_ms);
	// return a description of the Molecule
	std::string desc() const;
};

struct VQueue {

	void discard(Molecule* m);
	std::vector<Molecule>::iterator next();
	std::vector<Molecule>::iterator end() { return molecules[0].end(); }

	int schedule(Molecule* stopped);

	int enqueue(const Molecule& m, void* arg);
	int enqueue(const char* mdesc, void* arg);

	std::vector<Molecule> molecules[max_priority];
	int current_id;
};

void play_stop_handler(struct play *play, void *arg);

//-------------------------------------------------------------------------

static ausrc_st* g_rec;
static struct play *g_play;

static VQueue vqueue;

//-------------------------------------------------------------------------

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
		}
	}

	if (modestr.size() && modestr.back() == '|') {
		modestr.pop_back();
	}

	return modestr;
}

bool is_atom_start(const std::string &token) {
	if (token[0] == 'p' || token[0] == 'r' || token[0] == 'd') {
		return true;
	}

	return false;
}

size_t Play::set_filename(const std::string& filename) {

	struct aufile* au;
	struct aufile_prm prm;
	struct config_audio *cfg = &conf_config()->audio;

	_filename = filename;

	std::string path(cfg->audio_path);
	if (path.back() != '/') {
		path += "/";
	}
	path += filename;

	int err = aufile_open(&au, &prm, path.c_str(), AUFILE_READ);
	if (err) {
		return 0;
	}

	size_t length = aufile_get_length(au, &prm);

	mem_deref(au);

	return length;
}

// @pragma mark Molecule

size_t Molecule::length(int start, int end) const {
	size_t l = 0;

	if (end < 0) {
		end = atoms.size();
	}

	for (int i = start; i < end; ++i) {

		const Atom& a = atoms[i];

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

void Molecule::set_position(size_t position) {

	// search the correct atom/offset to select
	size_t l = 0;
	size_t l_prev = 0;

	if (mode & m_loop) {
		position %= length();
	}

	for (size_t i = 0; i < atoms.size(); ++i) {

		Atom& a = atoms[i];

		if (std::holds_alternative<Play>(a)) {
			l += std::get<Play>(a).length();
		}
		else if (std::holds_alternative<DTMF>(a)) {
			l += std::get<DTMF>(a).length();
		}
		else if (std::holds_alternative<Record>(a)) {
			l += std::get<Record>(a).length();
		}

		if (l >= position) {
			if (mode & m_mute) {

				size_t offset = (position - l_prev);

				current = i;

				if (std::holds_alternative<Play>(a)) {
					std::get<Play>(a).set_offset(offset);
				}
				else if (std::holds_alternative<DTMF>(a)) {
					std::get<DTMF>(a).set_offset(offset);
				}

				return;
			}
		}

		l_prev = l;
	}
}

std::string Molecule::desc() const {
	std::string desc = std::to_string(priority) + ' ' + mode_string(mode);

	for (auto a: atoms) {
		if (std::holds_alternative<Play>(a)) {
			const Play& p = std::get<Play>(a);
			desc += " play " + p.filename();
		}
		else if (std::holds_alternative<DTMF>(a)) {
			const DTMF& d = std::get<DTMF>(a);
			desc += " dtmf " + d.dtmf();
		}
		else if (std::holds_alternative<Record>(a)) {
			const Record& r = std::get<Record>(a);
			desc += " record " + r.filename();
		}
	}

	return desc;
}

// @pragma mark VQueue

void VQueue::discard(Molecule* m) {
	for (auto i = molecules[m->priority].begin();
		i != molecules[m->priority].end(); ++i) {

		if (m == &(*i)) {
			molecules[m->priority].erase(i);
			break;
		}
	}
}

std::vector<Molecule>::iterator VQueue::next() {

	for (int p = max_priority; p >= 0; --p) {

		for (std::vector<Molecule>::iterator i = molecules[p].begin(); i != molecules[p].end(); ++i) {

			if (i->atoms.size()) {
				return i;
			}
		}
	}

	return end();
}

int VQueue::schedule(Molecule* stopped) {

	struct config *cfg = conf_config();
	size_t now = tmr_jiffies();

	std::vector<Molecule>::iterator n = next();

	if (stopped) {
		// Just remove Molecules with m_discard
		if (stopped->mode & m_discard) {
			discard(stopped);
			n = next();
		}
		else if (n != end() && stopped == &(*n)) {
			// The current molecule was stopped
			++n->current;
			if (n->current >= n->atoms.size()) {
				discard(stopped);
				n = next();
			}
		}
	}

	while (n != end()) {

		if (n->mode & m_pause) {
			n->set_position(n->position);
		}
		else {
			size_t pos = now - n->time_stopped;

			if (n->mode & m_mute) {

				if (pos >= n->length()) {
					if (n->mode & m_mute) {
						discard(&(*n));
						n = next();
					}
					else if (n->mode & m_loop) {
						pos = pos % n->length();
					}

					n->set_position(pos);
					break;
				}
			}
		}

		Atom a = n->atoms[n->current];

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
			play_set_finish_handler(g_play, play_stop_handler, (void*)&(*n));
		}
		else if (std::holds_alternative<DTMF>(a)) {

			DTMF& d = std::get<DTMF>(a);

			if (++d > d.size()) {
				d.reset();
				n->current++;
				if (n->current >= n->atoms.size()) {
					return schedule(&(*n));
				}
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
			play_set_finish_handler(g_play, play_stop_handler, &(*n));
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

		n = vqueue.next();
	}

	if (n != end() && n->current == 0) {
		n->time_started = tmr_jiffies();
	}

	return 0;
}

int VQueue::enqueue(const Molecule& m, void* arg) {

	vqueue.molecules[m.priority].push_back(m);

	/* Stop the current player or recorder, if any */
	g_play = (struct play*)mem_deref(g_play);
	g_rec = (struct ausrc_st*)mem_deref(g_rec);

	auto stopped = next();
	if (stopped != end()) {
		return schedule(&(*stopped));
	}

	return schedule(nullptr);
}

int VQueue::enqueue(const char* mdesc, void* arg) {

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


void play_stop_handler(struct play *play, void *arg) {

	info("play file previous\n");
	size_t now = tmr_jiffies();

	/* Stop the current player or recorder, if any */
	g_play = (struct play*)mem_deref(g_play);
	g_rec = (struct ausrc_st*)mem_deref(g_rec);

	Molecule* stopped = (Molecule*)arg;

	stopped->time_stopped = now;
	stopped->position = now - stopped->time_started;

	vqueue.schedule((Molecule*)arg);
}
