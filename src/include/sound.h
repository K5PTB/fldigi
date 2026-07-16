// ----------------------------------------------------------------------------
//
//      sound.h
//
// Copyright (C) 2006-2007
//              Dave Freese, W1HKJ
//
// Copyright (C) 2007-2009
//              Stelios Bounanos, M0GLD
//
// This file is part of fldigi.
//
// Fldigi is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// Fldigi is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with fldigi.  If not, see <http://www.gnu.org/licenses/>.
// ----------------------------------------------------------------------------

#ifndef _SOUND_H
#define _SOUND_H

#include <iostream>

#include <string>
#include <cstring>
#include <climits>
#include <vector>

#  include <sndfile.h>

#include <samplerate.h>

#define SCBLOCKSIZE 512


class SndException : public std::exception
{
public:
	SndException(int err_ = 0)
		: err(err_), msg(std::string("Sound error: ") + err_to_str(err_))
	{ }
	SndException(const char* msg_)
		: err(1), msg(msg_)
	{ }
	SndException(int err_, const std::string& msg_) : err(err_), msg(msg_) { }
	virtual ~SndException() throw() { }

	const char*	what(void) const throw() { return msg.c_str(); }
	int		error(void) const { return err; }

protected:
	const char* err_to_str(int e) { return strerror(e); }

	int		err;
	std::string	msg;
};


class SoundBase {

public:
	enum sndfile_state { ACTIVE, PAUSED };

protected:
	int		sample_frequency;
	int		txppm;
	int		rxppm;

	// for interface to the samplerate resampling library
	SRC_STATE	*tx_src_state;
	SRC_STATE	*rx_src_state;
	double		*wrt_buffer;

	SNDFILE* ofCapture;
	SNDFILE* ifPlayback;
	SNDFILE* ofGenerate;

	int			capture_state;
	int			playback_state;
	int			generate_state;

// 2 channel writes
	SRC_STATE	*writ_src_state_left;
	SRC_STATE	*writ_src_state_right;
	SRC_DATA	*writ_src_data_left;
	SRC_DATA	*writ_src_data_right;

	float		*src_write_buffer_left;
	float		*src_write_buffer_right;

// 1 channel reads
	SRC_STATE	*play_src_state;
	SRC_DATA	*play_src_data;

	float		*src_rd_out_buffer;
	float		*src_rd_inp_buffer;
	float		*out_pointer;

	double		req_sample_rate;

	SF_INFO		play_info;

	float modem_wr_sr;
	float modem_play_sr;

	bool   new_playback;
	sf_count_t  read_file(SNDFILE* file, float* buf, size_t count);
	void	write_file(SNDFILE* file, float* buf_left, float *buf_right, size_t count);
	void	write_file(SNDFILE* file, double* buf_left, double *buf_right, size_t count);

public:
	SoundBase();
	virtual ~SoundBase();
	virtual int		Open(int mode, int freq = 8000) = 0;
	virtual void	Close(unsigned dir = UINT_MAX) = 0;
	virtual void	Abort(unsigned dir = UINT_MAX) = 0;
	virtual size_t	Write(double *, size_t) = 0;
	virtual size_t	Write_stereo(double *, double *, size_t) = 0;
	virtual size_t	resample_write(float *buf, size_t count) = 0;
	virtual size_t	Read(float *, size_t) = 0;
	virtual void	flush(unsigned dir = UINT_MAX) = 0;
	virtual bool	must_close(int dir = 0) = 0;

	virtual int		startGenerate(std::string fname, int format);
	virtual void	pauseGenerate();
	virtual void	stopGenerate();

	virtual int		startPlayback(std::string fname, int format);
	virtual void	pausePlayback();
	virtual void	stopPlayback();

	virtual int		startCapture(std::string fname, int format);
	virtual void	pauseCapture();
	virtual void	stopCapture();

	int		AudioMP3(std::string fname);
	int		AudioWAV(std::string fname);
	int		Audio(std::string fname);
};


#if USE_OSS

class SoundOSS : public SoundBase {
private:
	std::string	device;
	int		device_fd;
	int		version;
	int		capability_mask;
	int		format_mask;
	int		channels;
	int		play_format;
	int		mode;
	bool	formatok;
	unsigned char	*cbuff;

	SRC_DATA	*rx_src_data;
	SRC_DATA	*tx_src_data;
	float		*snd_buffer;
	float		*src_buffer;

	void	getVersion();
	void	getCapabilities();
	void	getFormats();
	void	setfragsize();
	void	Channels(int);
	void	Frequency(int);
	void	Format(int);
	int		BufferSize(int);
	bool	wait_till_finished();
	bool	reset_device();

public:
	SoundOSS(const char *dev = "/dev/dsp");
	~SoundOSS();
	int		Open(int mode, int freq = 8000);
	void	Close(unsigned dir = UINT_MAX);
	void	Abort(unsigned dir = UINT_MAX) { Close(dir); }
	size_t		Write(double *, size_t);
	size_t		Write_stereo(double *, double *, size_t);
	size_t		resample_write(float *buf, size_t count);
	size_t		Read(float *, size_t);
	bool		must_close(int dir = 0) { return true; }
	void		flush(unsigned dir = UINT_MAX) { wait_till_finished(); }

private:
	int		Fd() { return device_fd; }
	int		Frequency() { return sample_frequency;};
	int		Version() {return version;};
	int		Capabilities() {return capability_mask;};
	int		Formats() { return format_mask;};
	int		Channels() { return channels;};
	int		Format() { return play_format;};
	bool	FormatOK() { return formatok;};
};

#endif // USE_OSS


#if USE_PORTAUDIO
#  include <pthread.h>
#  include <semaphore.h>
#  include <vector>
#  include <portaudio.h>
#  include "ringbuffer.h"

class SoundPort : public SoundBase
{
public:
        typedef std::vector<const PaDeviceInfo*>::const_iterator device_iterator;
        static void	initialize(void);
        static void	terminate(void);
        static const std::vector<const PaDeviceInfo*>& devices(void);
        static void devices_info(std::string& in, std::string& out);
	static const std::vector<double>& get_supported_rates(const std::string& name, unsigned dir);

public:
        SoundPort(const char *in_dev, const char *out_dev);
        ~SoundPort();
	int 		Open(int mode, int freq = 8000);
	void 		Close(unsigned dir = UINT_MAX);
	void 		Abort(unsigned dir = UINT_MAX);
	size_t 		Write(double *buf, size_t count);
	size_t		Write_stereo(double *bufleft, double *bufright, size_t count);
	size_t 		Read(float *buf, size_t count);
	bool		must_close(int dir = 0);
	void		flush(unsigned dir = UINT_MAX);

private:
        void		src_data_reset(unsigned dir);
        static long	src_read_cb(void* arg, float** data);
        size_t          resample_write(float* buf, size_t count);
	device_iterator name_to_device(std::string& name, unsigned dir);
        void 		init_stream(unsigned dir);
        void 		start_stream(unsigned dir);
        void 		pause_stream(unsigned dir);
        bool		stream_active(unsigned dir);
        bool		full_duplex_device(const PaDeviceInfo* dev);
        double		find_srate(unsigned dir);
	static void	probe_supported_rates(const device_iterator& idev);
        void		pa_perror(int err, const char* str = 0);
        static void	init_hostapi_ext(void);
        static PaStreamCallback stream_process;
        static PaStreamFinishedCallback stream_stopped;

private:
        static bool                             pa_init;
	static std::vector<const PaDeviceInfo*> devs;
        float* 					fbuf;
		float*					src_buffer;
	SRC_DATA	*tx_src_data;

        enum {
                spa_continue = paContinue, spa_complete = paComplete,
                spa_abort = paAbort, spa_drain, spa_pause
        };
        struct stream_data {
                std::string device;
                device_iterator idev;

                PaStream* stream;
                PaStreamParameters params;

                unsigned frames_per_buffer;
                double dev_sample_rate;
                double src_ratio;

                sem_t* rwsem;
		pthread_mutex_t* cmutex;
		pthread_cond_t* ccond;
                int state;
                ringbuffer<float>* rb;
		size_t blocksize;
                size_t advance;
        } sd[2];
};

class SndPortException : public SndException
{
public:
	SndPortException(int err_ = 0)
		: SndException(err_, std::string("PortAudio error: ") + err_to_str(err_))
	{ }
	SndPortException(const char* msg_) : SndException(msg_) { }
protected:
	const char* err_to_str(int e) { return Pa_GetErrorText(e); }
};

#endif // USE_PORTAUDIO


#if USE_PULSEAUDIO
#  include <pulse/simple.h>
#  include <pulse/error.h>
extern "C" { const char* pa_get_library_version(void); };

class SoundPulse : public SoundBase
{
public:
	SoundPulse(const char* dev);
	virtual ~SoundPulse();

	int	Open(int mode, int freq = 8000);
	void    Close(unsigned dir = UINT_MAX);
	void    Abort(unsigned dir = UINT_MAX);
	size_t	Write(double* buf, size_t count);
	size_t	Write_stereo(double* bufleft, double* bufright, size_t count);
	size_t	Read(float *buf, size_t count);
	bool	must_close(int dir = 0) { return false; }
	void	flush(unsigned dir = UINT_MAX);

private:
	void	src_data_reset(int mode);
        static long	src_read_cb(void* arg, float** data);
	size_t	resample_write(float* buf, size_t count);

private:
	struct stream_data {
		pa_simple*	stream;
		pa_sample_spec	stream_params;
		pa_buffer_attr  buffer_attrs;
		pa_stream_direction_t dir;
		double		src_ratio;
		size_t		blocksize;
	} sd[2];

	SRC_DATA* tx_src_data;
	float* fbuf;
	float* rbuf;
	float* snd_buffer;
	float* src_buffer;
};

class SndPulseException : public SndException
{
public:
	SndPulseException(int err_ = 0)
		: SndException(err_, std::string("PulseAudio error: ") + err_to_str(err_))
	{ }
	SndPulseException(const char* msg_) : SndException(msg_) { }
protected:
	const char* err_to_str(int e) { return pa_strerror(e); }
};

#endif // USE_PULSEAUDIO


// TCI (Transceiver Control Interface) audio backend, unconditionally
// available (no external dependency beyond libsamplerate, which the
// whole SoundBase family already requires). RX pulls decoded mono float
// samples out of tci_io.cxx's ring buffer via libsamplerate's callback
// API (rx_src_state), matching SoundPulse's ppm-correction pattern but
// used here for the primary rate conversion, since the TCI stream's rate
// (TCI_AUDIO_SAMPLE_RATE) generally does not match the modem's requested
// rate. TX (Write/Write_stereo) resamples modem audio up to
// TCI_AUDIO_SAMPLE_RATE (tx_src_state, block-based src_process) and pushes
// it into tci_io.cxx's TX ring buffer; actual sending is paced entirely by
// the server's TX_CHRONO frames, not by this class.
class SoundTCI : public SoundBase
{
public:
	SoundTCI();
	virtual ~SoundTCI();
	int		Open(int mode, int freq = 8000);
	void	Close(unsigned dir = UINT_MAX);
	void	Abort(unsigned dir = UINT_MAX) { Close(dir); }
	size_t	Write(double *, size_t);
	size_t	Write_stereo(double *, double *, size_t);
	size_t	resample_write(float *buf, size_t count);
	size_t	Read(float *, size_t);
	void	flush(unsigned dir = UINT_MAX);
	bool	must_close(int dir = 0) { return false; }

private:
	static long	src_read_cb(void* arg, float** data);
	float	*rx_snd_buffer;
	size_t	rx_blocksize;
	std::vector<float> tx_fbuf;
	std::vector<float> tx_outbuf;
	bool	rx_open;
	unsigned rx_conn_gen;
};

class SoundNull : public SoundBase
{
public:
	int	Open(int mode, int freq = 44100) { sample_frequency = freq; return 0; }
	void    Close(unsigned) { }
	void    Abort(unsigned) { }
	size_t	Write(double* buf, size_t count);
	size_t	Write_stereo(double* bufleft, double* bufright, size_t count);
	size_t	Read(float *buf, size_t count);
	size_t	resample_write(float* buf, size_t count){return 0;};
	bool	must_close(int dir = 0) { return false; }
	void	flush(unsigned);
};

namespace SND_SUPPORT {
	bool format_supported(int format);
	void get_file_params(std::string def_fname, std::string &fname, int &format, bool check);
	void tag_file(SNDFILE *sndfile, const char *title);
};


#endif // SOUND_H
