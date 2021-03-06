/*
 * (C) 2010-2013 Stefan Seyfried
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#include <errno.h>
#include <fcntl.h>
#include <malloc.h>
#include <unistd.h>
#include <sys/types.h>
#include <inttypes.h>
#include <cstdio>
#include <cstring>

#include <pthread.h>
#include <aio.h>

#include "record_hal.h"
#include "dmx_hal.h"
#include "lt_debug.h"
#define lt_debug(args...) _lt_debug(TRIPLE_DEBUG_RECORD, this, args)
#define lt_info(args...) _lt_info(TRIPLE_DEBUG_RECORD, this, args)

#define BUFSIZE (2 << 20) /* 2MB */
static const int bufsize = BUFSIZE;

typedef enum {
	RECORD_RUNNING,
	RECORD_STOPPED,
	RECORD_FAILED_READ,	/* failed to read from DMX */
	RECORD_FAILED_OVERFLOW,	/* cannot write fast enough */
	RECORD_FAILED_FILE,	/* cannot write to file */
	RECORD_FAILED_MEMORY	/* out of memory */
} record_state_t;

class RecData
{
public:
	RecData(int num) {
		dmx = NULL;
		buf = NULL;
		record_thread_running = false;
		file_fd = -1;
		exit_flag = RECORD_STOPPED;
		dmx_num = num;
		state = REC_STATUS_OK;
	}
	int file_fd;
	int dmx_num;
	cDemux *dmx;
	uint8_t *buf;
	pthread_t record_thread;
	bool record_thread_running;
	record_state_t exit_flag;
	int state;
	void RecordThread();
};


/* helper function to call the cpp thread loop */
void *execute_record_thread(void *c)
{
	RecData *obj = (RecData *)c;
	obj->RecordThread();
	return NULL;
}

cRecord::cRecord(int num)
{
	lt_info("%s %d\n", __func__, num);
	pd = new RecData(num);
}

cRecord::~cRecord()
{
	lt_info("%s: calling ::Stop()\n", __func__);
	Stop();
	delete pd;
	pd = NULL;
	lt_info("%s: end\n", __func__);
}

bool cRecord::Open(void)
{
	lt_info("%s\n", __func__);
	pd->exit_flag = RECORD_STOPPED;
	return true;
}

#if 0
// unused
void cRecord::Close(void)
{
	lt_info("%s: \n", __func__);
}
#endif

bool cRecord::Start(int fd, unsigned short vpid, unsigned short *apids, int numpids, uint64_t)
{
	lt_info("%s: fd %d, vpid 0x%03x\n", __func__, fd, vpid);
	int i;

	if (!pd->dmx)
		pd->dmx = new cDemux(pd->dmx_num);

	pd->dmx->Open(DMX_TP_CHANNEL, NULL, 2*1024*1024);
	pd->dmx->pesFilter(vpid);

	for (i = 0; i < numpids; i++)
		pd->dmx->addPid(apids[i]);

	pd->file_fd = fd;
	pd->exit_flag = RECORD_RUNNING;
	if (posix_fadvise(pd->file_fd, 0, 0, POSIX_FADV_DONTNEED))
		perror("posix_fadvise");

	pd->buf = (uint8_t *)malloc(bufsize);
	if (!pd->buf) {
		i = errno;
		lt_info("%s: unable to allocate read buffer of %d bytes (%m)\n", __func__, bufsize);
	}
	else
		i = pthread_create(&pd->record_thread, 0, execute_record_thread, pd);
	if (i != 0)
	{
		pd->exit_flag = RECORD_FAILED_READ;
		errno = i;
		lt_info("%s: error creating thread! (%m)\n", __func__);
		delete pd->dmx;
		pd->dmx = NULL;
		free(pd->buf);
		pd->buf = NULL;
		return false;
	}
	pd->record_thread_running = true;
	return true;
}

bool cRecord::Stop(void)
{
	lt_info("%s\n", __func__);

	if (pd->exit_flag != RECORD_RUNNING)
		lt_info("%s: status not RUNNING? (%d)\n", __func__, pd->exit_flag);

	pd->exit_flag = RECORD_STOPPED;
	if (pd->record_thread_running)
		pthread_join(pd->record_thread, NULL);
	pd->record_thread_running = false;

	if (pd->buf)
		free(pd->buf);
	pd->buf = NULL;

	/* We should probably do that from the destructor... */
	if (!pd->dmx)
		lt_info("%s: dmx == NULL?\n", __func__);
	else
		delete pd->dmx;
	pd->dmx = NULL;

	if (pd->file_fd != -1)
		close(pd->file_fd);
	else
		lt_info("%s: file_fd not open??\n", __func__);
	pd->file_fd = -1;
	return true;
}

bool cRecord::ChangePids(unsigned short /*vpid*/, unsigned short *apids, int numapids)
{
	std::vector<pes_pids> pids;
	cDemux *dmx = pd->dmx;
	int j;
	bool found;
	unsigned short pid;
	lt_info("%s\n", __func__);
	if (!dmx) {
		lt_info("%s: DMX = NULL\n", __func__);
		return false;
	}
	pids = dmx->pesfds;
	/* the first PID is the video pid, so start with the second PID... */
	for (std::vector<pes_pids>::const_iterator i = pids.begin() + 1; i != pids.end(); ++i) {
		found = false;
		pid = (*i).pid;
		for (j = 0; j < numapids; j++) {
			if (pid == apids[j]) {
				found = true;
				break;
			}
		}
		if (!found)
			dmx->removePid(pid);
	}
	for (j = 0; j < numapids; j++) {
		found = false;
		for (std::vector<pes_pids>::const_iterator i = pids.begin() + 1; i != pids.end(); ++i) {
			if ((*i).pid == apids[j]) {
				found = true;
				break;
			}
		}
		if (!found)
			dmx->addPid(apids[j]);
	}
	return true;
}

bool cRecord::AddPid(unsigned short pid)
{
	std::vector<pes_pids> pids;
	cDemux *dmx = pd->dmx;
	lt_info("%s: \n", __func__);
	if (!dmx) {
		lt_info("%s: DMX = NULL\n", __func__);
		return false;
	}
	pids = dmx->pesfds;
	for (std::vector<pes_pids>::const_iterator i = pids.begin(); i != pids.end(); ++i) {
		if ((*i).pid == pid)
			return true; /* or is it an error to try to add the same PID twice? */
	}
	return dmx->addPid(pid);
}

void RecData::RecordThread()
{
	lt_info("%s: begin\n", __func__);
	hal_set_threadname("hal:record");
	const int readsize = bufsize / 16;
	int buf_pos = 0;
	int queued = 0;
	struct aiocb a;

	int val = fcntl(file_fd, F_GETFL);
	if (fcntl(file_fd, F_SETFL, val|O_APPEND))
		lt_info("%s: O_APPEND? (%m)\n", __func__);

	memset(&a, 0, sizeof(a));
	a.aio_fildes = file_fd;
	a.aio_sigevent.sigev_notify = SIGEV_NONE;

	dmx->Start();
	int overflow_count = 0;
	bool overflow = false;
	int r = 0;
	while (exit_flag == RECORD_RUNNING)
	{
		if (buf_pos < bufsize)
		{
			if (overflow_count) {
				lt_info("%s: Overflow cleared after %d iterations\n", __func__, overflow_count);
				overflow_count = 0;
			}
			int toread = bufsize - buf_pos;
			if (toread > readsize)
				toread = readsize;
			ssize_t s = dmx->Read(buf + buf_pos, toread, 50);
			lt_debug("%s: buf_pos %6d s %6d / %6d\n", __func__,
				buf_pos, (int)s, bufsize - buf_pos);
			if (s < 0)
			{
				if (errno != EAGAIN && (errno != EOVERFLOW || !overflow))
				{
					lt_info("%s: read failed: %m\n", __func__);
					exit_flag = RECORD_FAILED_READ;
					state = REC_STATUS_OVERFLOW;
					break;
				}
			}
			else
			{
				overflow = false;
				buf_pos += s;
			}
		}
		else
		{
			if (!overflow)
				overflow_count = 0;
			overflow = true;
			if (!(overflow_count % 10))
				lt_info("%s: buffer full! Overflow? (%d)\n", __func__, ++overflow_count);
			state = REC_STATUS_SLOW;
		}
		r = aio_error(&a);
		if (r == EINPROGRESS)
		{
			lt_debug("%s: aio in progress, free: %d\n", __func__, bufsize - buf_pos);
			continue;
		}
		// not calling aio_return causes a memory leak  --martii
		r = aio_return(&a);
		if (r < 0)
		{
			exit_flag = RECORD_FAILED_FILE;
			lt_debug("%s: aio_return = %d (%m)\n", __func__, r);
			break;
		}
		else
			lt_debug("%s: aio_return = %d, free: %d\n", __func__, r, bufsize - buf_pos);
		if (posix_fadvise(file_fd, 0, 0, POSIX_FADV_DONTNEED))
			perror("posix_fadvise");
		if (queued)
		{
			memmove(buf, buf + queued, buf_pos - queued);
			buf_pos -= queued;
		}
		queued = buf_pos;
		a.aio_buf = buf;
		a.aio_nbytes = queued;
		r = aio_write(&a);
		if (r)
		{
			lt_info("%s: aio_write %d (%m)\n", __func__, r);
			exit_flag = RECORD_FAILED_FILE;
			break;
		}
	}
	dmx->Stop();
	while (true) /* write out the unwritten buffer content */
	{
		lt_debug("%s: run-out write, buf_pos %d\n", __func__, buf_pos);
		r = aio_error(&a);
		if (r == EINPROGRESS)
		{
			usleep(50000);
			continue;
		}
		r = aio_return(&a);
		if (r < 0)
		{
			exit_flag = RECORD_FAILED_FILE;
			lt_info("%s: aio_result: %d (%m)\n", __func__, r);
			break;
		}
		if (!queued)
			break;
		memmove(buf, buf + queued, buf_pos - queued);
		buf_pos -= queued;
		queued = buf_pos;
		a.aio_buf = buf;
		a.aio_nbytes = queued;
		r = aio_write(&a);
	}

#if 0
	// TODO: do we need to notify neutrino about failing recording?
	CEventServer eventServer;
	eventServer.registerEvent2(NeutrinoMessages::EVT_RECORDING_ENDED, CEventServer::INITID_NEUTRINO, "/tmp/neutrino.sock");
	stream2file_status2_t s;
	s.status = exit_flag;
	strncpy(s.filename,basename(myfilename),512);
	s.filename[511] = '\0';
	strncpy(s.dir,dirname(myfilename),100);
	s.dir[99] = '\0';
	eventServer.sendEvent(NeutrinoMessages::EVT_RECORDING_ENDED, CEventServer::INITID_NEUTRINO, &s, sizeof(s));
	printf("[stream2file]: pthreads exit code: %i, dir: '%s', filename: '%s' myfilename: '%s'\n", exit_flag, s.dir, s.filename, myfilename);
#endif

	lt_info("%s: end\n", __func__);
	pthread_exit(NULL);
}

int cRecord::GetStatus()
{
	if (pd)
		return pd->state;
	return REC_STATUS_OK; /* should not happen */
}

void cRecord::ResetStatus()
{
	return;
}
