/***************************************************************************
 *   Copyright (C) 2008-2011 by Andrzej Rybczak                            *
 *   electricityispower@gmail.com                                          *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA.              *
 ***************************************************************************/

#include "visualizer.h"

#ifdef ENABLE_VISUALIZER

#include "global.h"

#include <cerrno>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <fcntl.h>
#include <sys/time.h>

using Global::MainStartY;
using Global::MainHeight;

Visualizer *myVisualizer = new Visualizer;

const int Visualizer::WindowTimeout = 1000/25; /* 25 fps */

void Visualizer::Init()
{
	w = new Window(0, MainStartY, COLS, MainHeight, "", Config.visualizer_color, brNone);
	
	ResetFD();
	itsSamples = Config.visualizer_in_stereo ? 4096 : 2048;
#	ifdef HAVE_FFTW3_H
	itsFFTResults = itsSamples/2+1;
	itsFreqsMagnitude = new unsigned[itsFFTResults];
	itsInput = static_cast<double *>(fftw_malloc(sizeof(double)*itsSamples));
	itsOutput = static_cast<fftw_complex *>(fftw_malloc(sizeof(fftw_complex)*itsFFTResults));
	itsPlan = fftw_plan_dft_r2c_1d(itsSamples, itsInput, itsOutput, FFTW_ESTIMATE);
#	endif // HAVE_FFTW3_H
	
	FindOutputID();
	
	isInitialized = 1;
}

void Visualizer::SwitchTo()
{
	using Global::myScreen;
	
	if (myScreen == this)
		return;
	
	if (!isInitialized)
		Init();
	
	if (hasToBeResized)
		Resize();
	
	if (myScreen != this && myScreen->isTabbable())
		Global::myPrevScreen = myScreen;
	myScreen = this;
	w->Clear();
	
	SetFD();
	
	itsTimer.tv_sec = 0;
	itsTimer.tv_usec = 0;
	
	if (itsFifo >= 0)
		Global::wFooter->SetTimeout(WindowTimeout);
	Global::RedrawHeader = 1;
}

void Visualizer::Resize()
{
	w->Resize(COLS, MainHeight);
	w->MoveTo(0, MainStartY);
	hasToBeResized = 0;
}

std::basic_string<my_char_t> Visualizer::Title()
{
	return U("Music visualizer");
}

void Visualizer::Update()
{
	if (itsFifo < 0)
		return;
	
	// PCM in format 44100:16:1 (for mono visualization) and 44100:16:2 (for stereo visualization) is supported
	int16_t buf[itsSamples];
	ssize_t data = read(itsFifo, buf, sizeof(buf));
	if (data < 0) // no data available in fifo
		return;
	
	if (itsOutputID != -1 && Global::Timer.tv_sec > itsTimer.tv_sec+Config.visualizer_sync_interval)
	{
		Mpd.DisableOutput(itsOutputID);
		usleep(50000);
		Mpd.EnableOutput(itsOutputID);
		gettimeofday(&itsTimer, 0);
	}
	
	void (Visualizer::*draw)(int16_t *, ssize_t, size_t, size_t);
#	ifdef HAVE_FFTW3_H
	if (!Config.visualizer_use_wave)
		draw = &Visualizer::DrawFrequencySpectrum;
	else
#	endif // HAVE_FFTW3_H
		draw = &Visualizer::DrawSoundWave;
	
	w->Clear();
	if (Config.visualizer_in_stereo)
	{
		ssize_t bytes_read = data/sizeof(int16_t);
		int16_t buf_left[bytes_read/2], buf_right[bytes_read/2];
		for (ssize_t i = 0, j = 0; i < bytes_read; i += 2, ++j)
		{
			buf_left[j] = buf[i];
			buf_right[j] = buf[i+1];
		}
		size_t half_height = MainHeight/2;
		(this->*draw)(buf_left, data/2, 0, half_height);
		(this->*draw)(buf_right, data/2, half_height+(draw == &Visualizer::DrawSoundWave ? 1 : 0), half_height+(draw != &Visualizer::DrawSoundWave ? 1 : 0));
	}
	else
		(this->*draw)(buf, data, 0, MainHeight);
	w->Refresh();
}

void Visualizer::SpacePressed()
{
#	ifdef HAVE_FFTW3_H
	Config.visualizer_use_wave = !Config.visualizer_use_wave;
	ShowMessage("Visualization type: %s", Config.visualizer_use_wave ? "Sound wave" : "Frequency spectrum");
#	endif // HAVE_FFTW3_H
}

void Visualizer::DrawSoundWave(int16_t *buf, ssize_t data, size_t y_offset, size_t height)
{
	const int samples_per_col = data/sizeof(int16_t)/COLS;
	const int half_height = height/2;
	*w << fmtAltCharset;
	double prev_point_pos = 0;
	for (int i = 0; i < COLS; ++i)
	{
		double point_pos = 0;
		for (int j = 0; j < samples_per_col; ++j)
			point_pos += buf[i*samples_per_col+j];
		point_pos /= samples_per_col;
		point_pos /= std::numeric_limits<int16_t>::max();
		point_pos *= half_height;
		*w << XY(i, y_offset+half_height+point_pos) << '`';
		if (i && abs(prev_point_pos-point_pos) > 2)
		{
			// if gap is too big. intermediate values are needed
			// since without them all we see are blinking points
			const int breakpoint = std::max(prev_point_pos, point_pos);
			const int half = (prev_point_pos+point_pos)/2;
			for (int k = std::min(prev_point_pos, point_pos)+1; k < breakpoint; k += 2)
					*w << XY(i-(k < half), y_offset+half_height+k) << '`';
		}
		prev_point_pos = point_pos;
	}
	*w << fmtAltCharsetEnd;
}

#ifdef HAVE_FFTW3_H
void Visualizer::DrawFrequencySpectrum(int16_t *buf, ssize_t data, size_t y_offset, size_t height)
{
	// zero old values
	std::fill(buf+data/sizeof(int16_t), buf+data/2, 0);
	for (unsigned i = 0; i < data/2; ++i)
		itsInput[i] = buf[i];
	
	fftw_execute(itsPlan);
	
	// count magnitude of each frequency and scale it to fit the screen
	for (unsigned i = 0; i < itsFFTResults; ++i)
		itsFreqsMagnitude[i] = sqrt(itsOutput[i][0]*itsOutput[i][0] + itsOutput[i][1]*itsOutput[i][1])/1e5*height/5;
	
	const int freqs_per_col = itsFFTResults/COLS /* cut bandwidth a little to achieve better look */ * 4/5;
	for (int i = 0; i < COLS; ++i)
	{
		size_t bar_height = 0;
		for (int j = 0; j < freqs_per_col; ++j)
			bar_height += itsFreqsMagnitude[i*freqs_per_col+j];
		bar_height = std::min(bar_height/freqs_per_col, height);
		mvwvline(w->Raw(), y_offset > 0 ? y_offset : height-bar_height, i, 0, bar_height);
	}
}
#endif // HAVE_FFTW3_H

void Visualizer::SetFD()
{
	if (itsFifo < 0 && (itsFifo = open(Config.visualizer_fifo_path.c_str(), O_RDONLY | O_NONBLOCK)) < 0)
		ShowMessage("Couldn't open \"%s\" for reading PCM data: %s", Config.visualizer_fifo_path.c_str(), strerror(errno));
}

void Visualizer::ResetFD()
{
	itsFifo = -1;
}

void Visualizer::FindOutputID()
{
	itsOutputID = -1;
	if (!Config.visualizer_output_name.empty())
	{
		MPD::OutputList outputs;
		Mpd.GetOutputs(outputs);
		for (unsigned i = 0; i < outputs.size(); ++i)
			if (outputs[i].first == Config.visualizer_output_name)
				itsOutputID = i;
		if (itsOutputID == -1)
			ShowMessage("There is no output named \"%s\"!", Config.visualizer_output_name.c_str());
	}
}

#endif // ENABLE_VISUALIZER

