/*
** Copyright 2002, Travis Geiselbrecht. All rights reserved.
** Distributed under the terms of the NewOS License.
*/
#include <kernel/kernel.h>
#include <kernel/console.h>
#include <kernel/debug.h>
#include <kernel/heap.h>
#include <kernel/int.h>
#include <kernel/vm.h>
#include <kernel/sem.h>
#include <kernel/lock.h>
#include <kernel/dev/fixed.h>
#include <kernel/fs/devfs.h>
#include <newos/errors.h>

#include <kernel/arch/cpu.h>
#include <kernel/arch/int.h>

#include <string.h>
#include <stdio.h>

#include "tty_priv.h"

#include <newos/tty_priv.h>

#if TTY_TRACE
#define TRACE(x) dprintf x
#else
#define TRACE(x)
#endif

tty_global thetty;

tty_desc *allocate_new_tty(void)
{
	int i;
	tty_desc *tty = NULL;

	mutex_lock(&thetty.lock);

	for(i=0; i<NUM_TTYS; i++) {
		if(thetty.ttys[i].inuse == false) {
			ASSERT(thetty.ttys[i].ref_count == 0);
			thetty.ttys[i].inuse = true;
			thetty.ttys[i].ref_count = 1;
			tty = &thetty.ttys[i];
			break;
		}
	}

	mutex_unlock(&thetty.lock);

	return tty;
}

void inc_tty_ref(tty_desc *tty)
{
	mutex_lock(&thetty.lock);
	tty->ref_count++;
	if(tty->ref_count == 1)
		tty->inuse = true;
	mutex_unlock(&thetty.lock);
}

void dec_tty_ref(tty_desc *tty)
{
	mutex_lock(&thetty.lock);
	tty->ref_count--;
	if(tty->ref_count == 0)
		tty->inuse = false;
	mutex_unlock(&thetty.lock);
}

static int tty_insert_char(struct line_buffer *lbuf, char c, bool move_line_start)
{
	bool was_empty = (AVAILABLE_READ(lbuf) == 0);

	// poke data into the endpoint
	lbuf->buffer[lbuf->head] = c;
	INC_HEAD(lbuf);
	if(move_line_start)
		lbuf->line_start = lbuf->head;
	if(was_empty && AVAILABLE_READ(lbuf) > 0)
		sem_release(lbuf->read_sem, 1);

	return 0;
}

int tty_ioctl(tty_desc *tty, int op, void *buf, size_t len)
{
	int err;

	mutex_lock(&tty->lock);

	switch(op) {
		case _TTY_IOCTL_GET_TTY_NUM:
			err = tty->index;
			break;
		case _TTY_IOCTL_GET_TTY_FLAGS: {
			struct tty_flags flags;

			flags.input_flags = tty->buf[ENDPOINT_MASTER_WRITE].flags;
			flags.output_flags = tty->buf[ENDPOINT_SLAVE_WRITE].flags;

			err = user_memcpy(buf, &flags, sizeof(flags));
			if(err < 0)
				break;

			err = 0;
			break;
		}
		case _TTY_IOCTL_SET_TTY_FLAGS: {
			struct tty_flags flags;

			err = user_memcpy(&flags, buf, sizeof(flags));
			if(err < 0)
				break;

			tty->buf[ENDPOINT_MASTER_WRITE].flags = flags.input_flags;
			tty->buf[ENDPOINT_SLAVE_WRITE].flags = flags.output_flags;

			err = 0;
			break;
		}
		default:
			err = ERR_INVALID_ARGS;
	}

	mutex_unlock(&tty->lock);

	return err;
}

ssize_t tty_read(tty_desc *tty, void *buf, ssize_t len, int endpoint)
{
	struct line_buffer *lbuf;
	ssize_t bytes_read = 0;
	ssize_t data_len;
	int err;

	if(len < 0) {
		bytes_read = ERR_INVALID_ARGS;
		goto err;
	}
	if(len == 0) {
		bytes_read = 0;
		goto err;
	}

	ASSERT(endpoint == ENDPOINT_MASTER_READ || endpoint == ENDPOINT_SLAVE_READ);
	lbuf = &tty->buf[endpoint];

	// wait for data in the buffer
	err = sem_acquire_etc(lbuf->read_sem, 1, SEM_FLAG_INTERRUPTABLE, 0, NULL);
	if(err == ERR_SEM_INTERRUPTED)
		return err;

	mutex_lock(&tty->lock);

	// quick sanity check
	ASSERT(lbuf->len > 0);
	ASSERT(lbuf->head < lbuf->len);
	ASSERT(lbuf->tail < lbuf->len);
	ASSERT(lbuf->line_start < lbuf->len);

	// figure out how much data is ready to be read
	data_len = AVAILABLE_READ(lbuf);
	len = min(data_len, len);	

	ASSERT(len > 0);

	while(len > 0) {
		ssize_t copy_len = len;

		if(lbuf->tail + copy_len > lbuf->len) {
			copy_len = lbuf->len - lbuf->tail;
		}

		err = user_memcpy((char *)buf + bytes_read, lbuf->buffer + lbuf->tail, copy_len);
		if(err < 0) {
			sem_release(lbuf->read_sem, 1);
			goto err;
		}

		// update the buffer pointers
		lbuf->tail = (lbuf->tail + copy_len) % lbuf->len;
		len -= copy_len;
		bytes_read += copy_len;
	}

	// is there more data available?
	if(AVAILABLE_READ(lbuf) > 0)
		sem_release(lbuf->read_sem, 1);

	// did it used to be full?
	if(data_len == lbuf->len - 1)
		sem_release(lbuf->write_sem, 1);

err:
	mutex_unlock(&tty->lock);

	return bytes_read;
}

ssize_t tty_write(tty_desc *tty, const void *buf, ssize_t len, int endpoint)
{
	struct line_buffer *lbuf;
	struct line_buffer *other_lbuf;
	int buf_pos = 0;
	ssize_t bytes_written = 0;
	ssize_t data_len;
	int err;
	char c;

	if(len < 0) {
		bytes_written = ERR_INVALID_ARGS;
		goto err;
	}
	if(len == 0) {
		bytes_written = 0;
		goto err;
	}

	ASSERT(endpoint == ENDPOINT_MASTER_WRITE || endpoint == ENDPOINT_SLAVE_WRITE);
	lbuf = &tty->buf[endpoint];
	other_lbuf = &tty->buf[(endpoint == ENDPOINT_MASTER_WRITE) ? ENDPOINT_SLAVE_WRITE : ENDPOINT_MASTER_WRITE];

restart:
	// wait on space in the circular buffer
	err = sem_acquire_etc(lbuf->write_sem, 1, SEM_FLAG_INTERRUPTABLE, 0, NULL);
	if(err == ERR_SEM_INTERRUPTED) {
		bytes_written = err;
		goto err;
	}

	mutex_lock(&tty->lock);
	
	// quick sanity check
	ASSERT(lbuf->len > 0);
	ASSERT(lbuf->head < lbuf->len);
	ASSERT(lbuf->tail < lbuf->len);
	ASSERT(lbuf->line_start < lbuf->len);

	if(AVAILABLE_WRITE(lbuf) == 0)
		goto exit_loop;

tty_write_special_state:
	// process any pending state
	while(lbuf->state != TTY_STATE_NORMAL) {
		bool wrote_char = false;

		TRACE(("tty_write: special state machine: tty %p, lbuf %p, state %d\n", tty, lbuf, lbuf->state));
		TRACE(("\tlbuf %p, head %d, tail %d, line_start %d\n", lbuf, lbuf->head, lbuf->tail, lbuf->line_start));
		switch(lbuf->state) {
			case TTY_STATE_NORMAL:
				// fall through and enter the regular write routine
				break;
			case TTY_STATE_WRITE_CR:
				// stick it in the ring buffer
				c = '\r';
				tty_insert_char(lbuf, c, false);
				wrote_char = true;

				lbuf->state = TTY_STATE_WRITE_LF;
				break;
			case TTY_STATE_WRITE_LF:
				// stick a LF in the ring buffer
				c = '\n';
				tty_insert_char(lbuf, c, true); // move the line ptr up, since we are done with the line
				wrote_char = true;
				
				lbuf->state = TTY_STATE_NORMAL;
				break;
			default:
				panic("tty_write: unhandled tty state %d on tty %p\n", lbuf->state, tty);
		}
		if(wrote_char) {
			if(lbuf->flags & TTY_FLAG_ECHO) {
				if(AVAILABLE_WRITE(other_lbuf) == 0) {
					// XXX deal with this case					
				}
				tty_insert_char(other_lbuf, c, true);
			}
		}				
		if(AVAILABLE_WRITE(lbuf) == 0) 
			goto exit_loop;

	}	

	while(buf_pos < len) {
		bool wrote_char = false;

		TRACE(("tty_write: regular loop: tty %p, lbuf %p, buf_pos %d, len %d\n", tty, lbuf, buf_pos, len));
		TRACE(("\tlbuf %p, head %d, tail %d, line_start %d\n", lbuf, lbuf->head, lbuf->tail, lbuf->line_start));
		if(AVAILABLE_WRITE(lbuf) == 0)
			goto exit_loop;
		// process this data one at a time
		err = user_memcpy(&c, (char *)buf + buf_pos, sizeof(c));	// XXX make this more efficient
		if(err < 0) {
			sem_release(lbuf->write_sem, 1);
			goto err;
		}
		buf_pos++; // advance to next character
		bytes_written++;

		if(lbuf->flags & TTY_FLAG_CANON) {
			// do line editing
			switch(c) {
				case '\n': // end of line
					if(lbuf->flags & TTY_FLAG_NLCR) {
						lbuf->state = TTY_STATE_WRITE_CR;
						goto tty_write_special_state;
					}
					// fall through and write it normally
				default:
					// stick it in the ring buffer
					tty_insert_char(lbuf, c, false);
					wrote_char = true;
					break;
				case 0x08: // backspace
					// back the head pointer up one if it can
					if(lbuf->head != lbuf->line_start) {
						DEC_HEAD(lbuf);
						wrote_char = true;
					}
					break;
				case '\r': // CR
				case 0:
					// eat it
					break;
			}
		} else {
			// no line editing
			switch(c) {
				case '\n': // end of line
					if(lbuf->flags & TTY_FLAG_NLCR) {
						lbuf->state = TTY_STATE_WRITE_CR;
						goto tty_write_special_state;
					}					
					// fall through and write it normally
				default:
					tty_insert_char(lbuf, c, true);
					wrote_char = true;
					break;
			}
					
		}
		if(wrote_char) {
			if(lbuf->flags & TTY_FLAG_ECHO) {
				if(AVAILABLE_WRITE(other_lbuf) == 0) {
					// XXX deal with this case					
				}
				tty_insert_char(other_lbuf, c, true);
			}
		}
	}

exit_loop:
	// is there more space available?
	if(AVAILABLE_WRITE(lbuf) > 0)
		sem_release(lbuf->write_sem, 1);

	// did we process everything for the request?
	if(buf_pos < len) {
		mutex_unlock(&tty->lock);
		goto restart;	
	}

err:
	mutex_unlock(&tty->lock);
	
	return bytes_written;
}

int tty_dev_init(kernel_args *ka)
{
	int i, j;
	int err;

	// setup the global tty structure
	memset(&thetty, 0, sizeof(thetty));
	err = mutex_init(&thetty.lock, "tty master lock");
	if(err < 0)
		panic("could not create master tty lock\n");

	// set up the individual tty nodes
	for(i=0; i<NUM_TTYS; i++) {
		thetty.ttys[i].inuse = false;
		thetty.ttys[i].index = i;
		thetty.ttys[i].ref_count = 0;
		if(mutex_init(&thetty.ttys[i].lock, "tty lock") < 0)
			panic("couldn't create tty lock\n");

		// set up the two buffers (one for each direction)
		for(j=0; j<2; j++) {
			thetty.ttys[i].buf[j].read_sem = sem_create(0, "tty read sem");
			if(thetty.ttys[i].buf[j].read_sem < 0)
				panic("couldn't create tty read sem\n");
			thetty.ttys[i].buf[j].write_sem = sem_create(1, "tty write sem");
			if(thetty.ttys[i].buf[j].write_sem < 0)
				panic("couldn't create tty write sem\n");

			thetty.ttys[i].buf[j].head = 0;
			thetty.ttys[i].buf[j].tail = 0;
			thetty.ttys[i].buf[j].line_start = 0;
			thetty.ttys[i].buf[j].len = TTY_BUFFER_SIZE;
			thetty.ttys[i].buf[j].state = TTY_STATE_NORMAL;
			if(j == ENDPOINT_SLAVE_WRITE)
				thetty.ttys[i].buf[j].flags = TTY_FLAG_NLCR; // slave writes to this one, translate LR to CRLF
			else if(j == ENDPOINT_MASTER_WRITE)
				thetty.ttys[i].buf[j].flags = TTY_FLAG_CANON | TTY_FLAG_ECHO | TTY_FLAG_NLCR; // master writes into this one. do line editing and echo back
		}
	}

	// create device node
	devfs_publish_device("tty/master", NULL, &ttym_hooks);

	for(i=0; i<NUM_TTYS; i++) {
		char buf[128];

		sprintf(buf, "tty/slave/%d", i);
		devfs_publish_device(buf, &thetty.ttys[i], &ttys_hooks);
	}

	return 0;
}
