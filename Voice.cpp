/*
*   Copyright (C) 2017,2018.2021 by Jonathan Naylor G4KLX
*
*   This program is free software; you can redistribute it and/or modify
*   it under the terms of the GNU General Public License as published by
*   the Free Software Foundation; either version 2 of the License, or
*   (at your option) any later version.
*
*   This program is distributed in the hope that it will be useful,
*   but WITHOUT ANY WARRANTY; without even the implied warranty of
*   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*   GNU General Public License for more details.
*
*   You should have received a copy of the GNU General Public License
*   along with this program; if not, write to the Free Software
*   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
*/

#include "Voice.h"
#include "M17Defines.h"
#include "Log.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cassert>

#include <sys/stat.h>

const unsigned int SILENCE_LENGTH = 4U;

const unsigned char BIT_MASK_TABLE[] = { 0x80U, 0x40U, 0x20U, 0x10U, 0x08U, 0x04U, 0x02U, 0x01U };

#define WRITE_BIT1(p,i,b) p[(i)>>3] = (b) ? (p[(i)>>3] | BIT_MASK_TABLE[(i)&7]) : (p[(i)>>3] & ~BIT_MASK_TABLE[(i)&7])
#define READ_BIT1(p,i)    (p[(i)>>3] & BIT_MASK_TABLE[(i)&7])

CVoice::CVoice(const std::string& directory, const std::string& language, const std::string& callsign) :
m_language(language),
m_indxFile(),
m_m17File(),
m_callsign(callsign),
m_status(VS_NONE),
m_timer(1000U, 1U),
m_stopWatch(),
m_sent(0U),
m_m17(NULL),
m_voiceData(NULL),
m_voiceLength(0U),
m_positions()
{
	assert(!directory.empty());
	assert(!language.empty());

#if defined(_WIN32) || defined(_WIN64)
	m_indxFile = directory + "\\" + language + ".indx";
	m_m17File  = directory + "\\" + language + ".m17";
#else
	m_indxFile = directory + "/" + language + ".indx";
	m_m17File  = directory + "/" + language + ".m17";
#endif
}

CVoice::~CVoice()
{
	for (std::unordered_map<std::string, CPositions*>::iterator it = m_positions.begin(); it != m_positions.end(); ++it)
		delete it->second;

	m_positions.clear();

	delete[] m_m17;
	delete[] m_voiceData;
}

bool CVoice::open()
{
	FILE* fpindx = ::fopen(m_indxFile.c_str(), "rt");
	if (fpindx == NULL) {
		LogError("Unable to open the index file - %s", m_indxFile.c_str());
		return false;
	}

	struct stat statStruct;
	int ret = ::stat(m_m17File.c_str(), &statStruct);
	if (ret != 0) {
		LogError("Unable to stat the M17 file - %s", m_m17File.c_str());
		::fclose(fpindx);
		return false;
	}

	FILE* fpm17 = ::fopen(m_m17File.c_str(), "rb");
	if (fpm17 == NULL) {
		LogError("Unable to open the M17 file - %s", m_m17File.c_str());
		::fclose(fpindx);
		return false;
	}

	m_m17 = new unsigned char[statStruct.st_size];

	size_t sizeRead = ::fread(m_m17, 1U, statStruct.st_size, fpm17);
	if (sizeRead != 0U) {
		char buffer[80U];
		while (::fgets(buffer, 80, fpindx) != NULL) {
			char* p1 = ::strtok(buffer, "\t\r\n");
			char* p2 = ::strtok(NULL, "\t\r\n");
			char* p3 = ::strtok(NULL, "\t\r\n");

			if (p1 != NULL && p2 != NULL && p3 != NULL) {
				std::string symbol  = std::string(p1);
				unsigned int start  = ::atoi(p2);
				unsigned int length = ::atoi(p3);

				CPositions* pos = new CPositions;
				pos->m_start = start;
				pos->m_length = length;

				m_positions[symbol] = pos;
			}
		}
	}

	::fclose(fpindx);
	::fclose(fpm17);

	LogInfo("Loaded the audio and index file for %s", m_language.c_str());

	return true;
}

void CVoice::linkedTo(const std::string& reflector)
{
	std::vector<std::string> words;
	if (m_positions.count("linkedto") == 0U) {
		words.push_back("linked");
		words.push_back("2");
	} else {
		words.push_back("linkedto");
	}

	for (std::string::const_iterator it = reflector.cbegin(); it != reflector.cend(); ++it)
		words.push_back(std::string(1U, *it));

	createVoice(words);
}

void CVoice::unlinked()
{
	std::vector<std::string> words;
	words.push_back("notlinked");

	createVoice(words);
}

void CVoice::createVoice(const std::vector<std::string>& words)
{
	unsigned int m17Length = 0U;
	for (std::vector<std::string>::const_iterator it = words.begin(); it != words.end(); ++it) {
		if (m_positions.count(*it) > 0U) {
			CPositions* position = m_positions.at(*it);
			m17Length += position->m_length;
		} else {
			LogWarning("Unable to find character/phrase \"%s\" in the index", (*it).c_str());
		}
	}

	// Ensure that the Codec 2 audio is an integer number of M17 frames
	if ((m17Length % 2U) != 0U)
		m17Length++;

	// Add space for silence before and after the voice
	m17Length += SILENCE_LENGTH;
	m17Length += SILENCE_LENGTH;

	m_voiceData = new unsigned char[m17Length * M17_NETWORK_FRAME_LENGTH];

	// Start with silence
	m_voiceLength = 0U;
	for (unsigned int i = 0U; i < SILENCE_LENGTH; i++)
		createFrame(M17_3200_SILENCE, 1U);

	for (std::vector<std::string>::const_iterator it = words.begin(); it != words.end(); ++it) {
		if (m_positions.count(*it) > 0U) {
			CPositions* position = m_positions.at(*it);
			unsigned int start  = position->m_start;
			unsigned int length = position->m_length;
			createFrame(m_m17 + start, length);
		}
	}

	// End with silence
	for (unsigned int i = 0U; i < SILENCE_LENGTH; i++)
		createFrame(M17_3200_SILENCE, 1U);
}

unsigned int CVoice::read(unsigned char* data)
{
	assert(data != NULL);

	if (m_status != VS_SENDING)
		return 0U;

	unsigned int count = m_stopWatch.elapsed() / M17_FRAME_TIME;

	if (m_sent < count) {
		unsigned int offset = m_sent * M17_NETWORK_FRAME_LENGTH;
		::memcpy(data, m_voiceData + offset, M17_NETWORK_FRAME_LENGTH);

		offset += M17_NETWORK_FRAME_LENGTH;
		m_sent++;

		if (offset >= m_voiceLength) {
			m_timer.stop();
			m_voiceLength = 0U;
			delete[] m_voiceData;
			m_voiceData = NULL;
			m_status = VS_NONE;
		}

		return M17_NETWORK_FRAME_LENGTH;
	}

	return 0U;
}

void CVoice::eof()
{
	if (m_voiceLength == 0U)
		return;

	m_status = VS_WAITING;

	m_timer.start();
}

void CVoice::clock(unsigned int ms)
{
	m_timer.clock(ms);
	if (m_timer.isRunning() && m_timer.hasExpired()) {
		if (m_status == VS_WAITING) {
			m_stopWatch.start();
			m_status = VS_SENDING;
			m_sent = 0U;
		}
	}
}

void CVoice::createFrame(const unsigned char* audio, unsigned int length)
{
	assert(audio != NULL);
	assert(length > 0U);

	for (unsigned int i = 0U; i < length; i++) {
		// Create an M17 network frame
		m_voiceLength += M17_NETWORK_FRAME_LENGTH;
	}
}
