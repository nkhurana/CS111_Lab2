#include <linux/version.h>
#include <linux/autoconf.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/init.h>

#include <linux/sched.h>
#include <linux/kernel.h>  /* printk() */
#include <linux/errno.h>   /* error codes */
#include <linux/types.h>   /* size_t */
#include <linux/vmalloc.h>
#include <linux/genhd.h>
#include <linux/blkdev.h>
#include <linux/wait.h>
#include <linux/file.h>

#include "spinlock.h"
#include "osprd.h"

/* The size of an OSPRD sector. */
#define SECTOR_SIZE	512

/* This flag is added to an OSPRD file's f_flags to indicate that the file
 * is locked. */
#define F_OSPRD_LOCKED	0x80000

/* eprintk() prints messages to the console.
 * (If working on a real Linux machine, change KERN_NOTICE to KERN_ALERT or
 * KERN_EMERG so that you are sure to see the messages.  By default, the
 * kernel does not print all messages to the console.  Levels like KERN_ALERT
 * and KERN_EMERG will make sure that you will see messages.) */
#define eprintk(format, ...) printk(KERN_NOTICE format, ## __VA_ARGS__)

MODULE_LICENSE("Dual BSD/GPL");
MODULE_DESCRIPTION("CS 111 RAM Disk");
// EXERCISE: Pass your names into the kernel as the module's authors.
MODULE_AUTHOR("Neeraj Khurana & Evan Shi");

#define OSPRD_MAJOR	222

/* This module parameter controls how big the disk will be.
 * You can specify module parameters when you load the module,
 * as an argument to insmod: "insmod osprd.ko nsectors=4096" */
static int nsectors = 32;
module_param(nsectors, int, 0);

typedef struct read_list_node {
	pid_t reader;
	struct read_list_node *next;
} read_list_node;

typedef read_list_node* read_list_t;

typedef struct dead_tix_node {
	unsigned ticket;
	struct dead_tix_node *next;
} dead_tix_node;

typedef dead_tix_node* dead_tix_t;

/* The internal representation of our device. */
typedef struct osprd_info {
	uint8_t *data;                  // The data array. Its size is
	                                // (nsectors * SECTOR_SIZE) bytes.

	osp_spinlock_t mutex;           // Mutex for synchronizing access to
					// this block device

	unsigned ticket_head;		// Currently running ticket for
					// the device lock

	unsigned ticket_tail;		// Next available ticket for
					// the device lock

	wait_queue_head_t blockq;       // Wait queue for tasks blocked on
					// the device lock
    
	/* HINT: You may want to add additional fields to help
	         in detecting deadlock. */
			 
	//Added members
    int ramdisk_WriteLocked; //1 if true
    pid_t pid_holdingWriteLock;
    
    int num_ReadLocks;
	
	read_list_t read_list;
	dead_tix_t dead_tix;

	// The following elements are used internally; you don't need
	// to understand them.
	struct request_queue *queue;    // The device request queue.
	spinlock_t qlock;		// Used internally for mutual
	                                //   exclusion in the 'queue'.
	struct gendisk *gd;             // The generic disk.
} osprd_info_t;

#define NOSPRD 4
static osprd_info_t osprds[NOSPRD];


// Declare useful helper functions

/*
 * file2osprd(filp)
 *   Given an open file, check whether that file corresponds to an OSP ramdisk.
 *   If so, return a pointer to the ramdisk's osprd_info_t.
 *   If not, return NULL.
 */
static osprd_info_t *file2osprd(struct file *filp);

/*
 * for_each_open_file(task, callback, user_data)
 *   Given a task, call the function 'callback' once for each of 'task's open
 *   files.  'callback' is called as 'callback(filp, user_data)'; 'filp' is
 *   the open file, and 'user_data' is copied from for_each_open_file's third
 *   argument.
 */
static void for_each_open_file(struct task_struct *task,
			       void (*callback)(struct file *filp,
						osprd_info_t *user_data),
			       osprd_info_t *user_data);


/*
 * osprd_process_request(d, req)
 *   Called when the user reads or writes a sector.
 *   Should perform the read or write, as appropriate.
 */
static void osprd_process_request(osprd_info_t *d, struct request *req)
{
	if (!blk_fs_request(req)) {
		end_request(req, 0);
		return;
	}

	// EXERCISE: Perform the read or write request by copying data between
	// our data array and the request's buffer.
	// Hint: The 'struct request' argument tells you what kind of request
	// this is, and which sectors are being read or written.
	// Read about 'struct request' in <linux/blkdev.h>.
	// Consider the 'req->sector', 'req->current_nr_sectors', and
	// 'req->buffer' members, and the rq_data_dir() function.

	// Your code here.
	//eprintk("Should process request...\n");

    int byteOffset = req->sector*SECTOR_SIZE;
    int numBytes = req->current_nr_sectors*SECTOR_SIZE;
    
    //ensure writing and reading within bounds
    if (req->sector + req->current_nr_sectors > nsectors)
    {
        eprintk("Accessing out of bounds");
        end_request(req,0);
    }
    
    if (rq_data_dir(req)== READ)
    {
        //destination,source,#bytes
        memcpy(req->buffer,d->data+byteOffset,numBytes);
    }
    else if (rq_data_dir(req)== WRITE) //WRITE
    {
        memcpy(d->data+byteOffset,req->buffer,numBytes);
        
    }
    else end_request(req, 0);
    
	end_request(req, 1);
}


// This function is called when a /dev/osprdX file is opened.
// You aren't likely to need to change this.
static int osprd_open(struct inode *inode, struct file *filp)
{
	// Always set the O_SYNC flag. That way, we will get writes immediately
	// instead of waiting for them to get through write-back caches.
	filp->f_flags |= O_SYNC;
	return 0;
}

static int osprd_wake_cond(osprd_info_t *d, int dir, unsigned localTicket)
{
	osp_spin_lock(&d->mutex);
	while(d->dead_tix != NULL)
	{		
		if (d->dead_tix->ticket == d->ticket_tail)
		{
			dead_tix_t del = d->dead_tix;
			d->dead_tix = d->dead_tix->next;
			kfree(del);
		}
		else // not in head (must be greater than 1 element)
		{
			dead_tix_t itr = d->dead_tix;
			while(itr->next != NULL && itr->next->ticket != d->ticket_tail)
				itr = itr->next;
			
			if (itr->next != NULL)
			{
				dead_tix_t del = itr->next;
				itr->next = del->next;
				kfree(del);
			}
			else
				break;;
		}
		d->ticket_tail++;
	}
		
	int r;
	if (dir == READ)
	{
		r = (!(d->ramdisk_WriteLocked) && d->ticket_tail==localTicket);
		//eprintk("TICKET TAIL: %d\n", d->ticket_tail);
		//eprintk("LOCAL TICKET: %d\n", localTicket);
	}
	else
	{
		r = (d->num_ReadLocks==0 && !(d->ramdisk_WriteLocked) && d->ticket_tail==localTicket);
	}
	osp_spin_unlock(&d->mutex);
	//eprintk("PID %d COND VALUE: %d\n", current->pid, r);
	return r;
}

// This function is called when a /dev/osprdX file is finally closed.
// (If the file descriptor was dup2ed, this function is called only when the
// last copy is closed.)
static int osprd_close_last(struct inode *inode, struct file *filp)
{
	if (filp) {
		osprd_info_t *d = file2osprd(filp);
		int filp_writable = filp->f_mode & FMODE_WRITE;

		// EXERCISE: If the user closes a ramdisk file that holds
		// a lock, release the lock.  Also wake up blocked processes
		// as appropriate.

		// Your code here.

		// This line avoids compiler warnings; you may remove it.
		(void) filp_writable, (void) d;
        
        osp_spin_lock(&d->mutex);
        //could also just check if filp->flag is locked
        if (!d->ramdisk_WriteLocked && d->num_ReadLocks==0)
        {
            osp_spin_unlock(&d->mutex);
            return -EINVAL;
        }
        
        
        if (filp_writable)
        {
            d->ramdisk_WriteLocked=0;
            d->pid_holdingWriteLock=-1; //ensure no process holds write lock
			//eprintk("PID %d RELEASED WRITE LOCK\n", current->pid);
        }
        else //ramdisk was locked on reading
        {
            if (d->read_list != NULL)
			{
				//remove current pid from read_pid list	
				if (d->read_list->reader == current->pid) // read lock in head
				{
					read_list_t del = d->read_list;
					d->read_list = d->read_list->next;
					kfree(del);
				}
				else // not in head (must be greater than 1 element)
				{
					read_list_t itr = d->read_list;
					while(itr->next != NULL && itr->next->reader != current->pid)
						itr = itr->next;
					
					if (itr->next != NULL)
					{
						read_list_t del = itr->next;
						itr->next = itr->next->next;
						kfree(del);
					}
				}
				d->num_ReadLocks--;
				//eprintk("PID %d RELEASED READ LOCK\n", current->pid);
			}
        }
        
        //chnage filp->flag to unlocked (NEEDS TO BE DONE)
		wake_up_all(&d->blockq);
        filp->f_flags &= !F_OSPRD_LOCKED;
        osp_spin_unlock(&d->mutex);
	}

	return 0;
}


/*
 * osprd_lock
 */

/*
 * osprd_ioctl(inode, filp, cmd, arg)
 *   Called to perform an ioctl on the named file.
 */
int osprd_ioctl(struct inode *inode, struct file *filp,
		unsigned int cmd, unsigned long arg)
{
	osprd_info_t *d = file2osprd(filp);	// device info
	int r = 0;			// return value: initially 0

	// is file open for writing?
	int filp_writable = (filp->f_mode & FMODE_WRITE) != 0;

	// This line avoids compiler warnings; you may remove it.
	(void) filp_writable, (void) d;

	// Set 'r' to the ioctl's return value: 0 on success, negative on error

	if (cmd == OSPRDIOCACQUIRE) 
    {

		// EXERCISE: Lock the ramdisk.
		//
		// If *filp is open for writing (filp_writable), then attempt
		// to write-lock the ramdisk; otherwise attempt to read-lock
		// the ramdisk.
		//
                // This lock request must block using 'd->blockq' until:
		// 1) no other process holds a write lock;
		// 2) either the request is for a read lock, or no other process
		//    holds a read lock; and
		// 3) lock requests should be serviced in order, so no process
		//    that blocked earlier is still blocked waiting for the
		//    lock.
		//
		// If a process acquires a lock, mark this fact by setting
		// 'filp->f_flags |= F_OSPRD_LOCKED'.  You also need to
		// keep track of how many read and write locks are held:
		// change the 'osprd_info_t' structure to do this.
		//
		// Also wake up processes waiting on 'd->blockq' as needed.
		//
		// If the lock request would cause a deadlock, return -EDEADLK.
		// If the lock request blocks and is awoken by a signal, then
		// return -ERESTARTSYS.
		// Otherwise, if we can grant the lock request, return 0.

		// 'd->ticket_head' and 'd->ticket_tail' should help you
		// service lock requests in order.  These implement a ticket
		// order: 'ticket_tail' is the next ticket, and 'ticket_head'
		// is the ticket currently being served.  You should set a local
		// variable to 'd->ticket_head' and increment 'd->ticket_head'.
		// Then, block at least until 'd->ticket_tail == local_ticket'.
		// (Some of these operations are in a critical section and must
		// be protected by a spinlock; which ones?)

		// Your code here (instead of the next two lines).
		//eprintk("Attempting to acquire\n");
		//r = -ENOTTY;
        

		osp_spin_lock(&d->mutex);
        //Since process wants lock, ensure process doesn't already have write lock or else you will block current process and therefore it can never release lock
        if (current->pid == d->pid_holdingWriteLock)
		{
			osp_spin_unlock(&d->mutex);
            return -EDEADLK;
		}
        if (filp_writable)
		{
			//muust iterate thorugh all read_lock pids and ensure you haven't read locked the ramdisk already or deadlock! (NEEDS TO BE DONE)
            read_list_t itr = d->read_list;
			while(itr != NULL)
			{
				if (itr->reader == current->pid)
				{
					osp_spin_unlock(&d->mutex);
					return -EDEADLK;
				}
				itr = itr->next;
			}
		}
		osp_spin_unlock(&d->mutex);
		
        //Critical Section surrounding getting and incrementing ticket_head
        osp_spin_lock(&d->mutex);
        unsigned localTicket = d->ticket_head;
        d->ticket_head++;
        osp_spin_unlock(&d->mutex);
        
        //check if file is open for writing and process desires write lock
        if (filp_writable)
        {
		    osp_spin_lock(&d->mutex);

            //can only get writing lock if no other processes have writing or reading lock and you are holding the correct ticket
            if (d->num_ReadLocks!=0 || d->ramdisk_WriteLocked || d->ticket_tail!=localTicket)
            {
                //block current process until above is false and current process holds the next ticket!
                osp_spin_unlock(&d->mutex);
                int w_e_i_retValue = wait_event_interruptible(d->blockq, osprd_wake_cond(d, WRITE, localTicket));
                if (w_e_i_retValue == -ERESTARTSYS) 
				{
					//eprintk("PROCESS %d RECEIVED SIGNAL\n", current->pid);
					osp_spin_lock(&d->mutex);
					dead_tix_t new = kmalloc(sizeof(dead_tix_node), GFP_ATOMIC);
					new->ticket = localTicket;
					new->next = d->dead_tix;
					d->dead_tix = new;
					osp_spin_unlock(&d->mutex);
					return -ERESTARTSYS;
				}
                osp_spin_lock(&d->mutex);
            }
            
            //allow current process to have the write lock and update struct values
            d->ramdisk_WriteLocked=1;
            d->pid_holdingWriteLock = current->pid;
			//eprintk("PID %d WRITE LOCK\n", current->pid);
            filp->f_flags |= F_OSPRD_LOCKED;
            osp_spin_unlock(&d->mutex);
        
        }
        else //file is opened for reading
        {
            osp_spin_lock(&d->mutex);
             //We already checked if current process has a write lock; now block current process if someone else has a write lock on ramdisk
            if (d->ramdisk_WriteLocked || d->ticket_tail!=localTicket)
            {
                osp_spin_unlock(&d->mutex);
                //block current Process until above is false
                int w_e_i_retValue = wait_event_interruptible(d->blockq, osprd_wake_cond(d, READ, localTicket));
                if (w_e_i_retValue == -ERESTARTSYS) 
				{
					//eprintk("PROCESS %d RECEIVED SIGNAL\n", current->pid);
					osp_spin_lock(&d->mutex);
					dead_tix_t new = kmalloc(sizeof(dead_tix_node), GFP_ATOMIC);
					new->ticket = localTicket;
					new->next = d->dead_tix;
					d->dead_tix = new;
					osp_spin_unlock(&d->mutex);
					return -ERESTARTSYS;
				}
                osp_spin_lock(&d->mutex);
            }
            
            //process can get a read lock on the ramdisk
            d->num_ReadLocks++;
            filp->f_flags |= F_OSPRD_LOCKED;
			//eprintk("PID %d READ LOCK\n", current->pid);
            
            //add current pid to linked list of pids with read locks
            if (d->read_list == NULL)
			{
				read_list_t new = kmalloc(sizeof(read_list_node), GFP_ATOMIC);
				new->reader = current->pid;
				new->next = NULL;
				d->read_list = new;
			}
			else
			{
				read_list_t new = kmalloc(sizeof(read_list_node), GFP_ATOMIC);
				new->reader = current->pid;
				new->next = d->read_list;
				d->read_list = new;
			}
            osp_spin_unlock(&d->mutex);
        
        }
        osp_spin_lock(&d->mutex);
        d->ticket_tail++;
        osp_spin_unlock(&d->mutex);
        
	} 
    
    else if (cmd == OSPRDIOCTRYACQUIRE) 
    {

		// EXERCISE: ATTEMPT to lock the ramdisk.
		//
		// This is just like OSPRDIOCACQUIRE, except it should never
		// block.  If OSPRDIOCACQUIRE would block or return deadlock,
		// OSPRDIOCTRYACQUIRE should return -EBUSY.
		// Otherwise, if we can grant the lock request, return 0.

		// Your code here (instead of the next two lines).
		//eprintk("Attempting to try acquire\n");
		//r = -ENOTTY;
        
        //Since process wants lock, ensure process doesn't already have write lock or else you will block current process and therefore it can never release lock
        if (current->pid == d->pid_holdingWriteLock)
            return -EDEADLK;
        
        
        //check if file is open for writing and process desires write lock
        if (filp_writable)
        {
		    osp_spin_lock(&d->mutex);
            //muust iterate thorugh all read_lock pids and ensure you haven't locked the ramdisk already or deadlock!
            read_list_t itr = d->read_list;
			while(itr != NULL)
			{
				if (itr->reader == current->pid)
				{
					osp_spin_unlock(&d->mutex);
					return -EDEADLK;
				}
				itr = itr->next;
			}

            //can only get writing lock if no other processes have writing or reading lock
            if (d->num_ReadLocks!=0 || d->ramdisk_WriteLocked || d->ticket_head!=d->ticket_tail)
            {
                //CANNOT ACQUIRE LOCK
                osp_spin_unlock(&d->mutex);
                return -EBUSY;
            }
            
            //allow current process to have the write lock and update struct values
            d->ramdisk_WriteLocked=1;
            d->pid_holdingWriteLock = current->pid;
			
            filp->f_flags |= F_OSPRD_LOCKED;
            osp_spin_unlock(&d->mutex);
            
        }
        else //file is opened for reading
        {
            //WHAT IF ALREADY HAVE READ LOCK?!?!??!
            //lets assume we don't need to take care of that case
            
            //We already checked if current process has a write lock; now ensure no one else has write lock
            osp_spin_lock(&d->mutex);
            if (d->ramdisk_WriteLocked || d->ticket_head!=d->ticket_tail)
            {
                osp_spin_unlock(&d->mutex);
                return -EBUSY;
            }
            
            //process can get a read lock on the ramdisk
            d->num_ReadLocks++;
            filp->f_flags |= F_OSPRD_LOCKED;

            //add current pid to linked list of pids with read locks (NEEDS TO BE DONE)
            if (d->read_list == NULL)
			{
				read_list_t new = kmalloc(sizeof(read_list_node), GFP_ATOMIC);
				new->reader = current->pid;
				new->next = NULL;
				d->read_list = new;
			}
			else
			{
				read_list_t new = kmalloc(sizeof(read_list_node), GFP_ATOMIC);
				new->reader = current->pid;
				new->next = d->read_list;
				d->read_list = new;
			}
            osp_spin_unlock(&d->mutex);
            
        }

	} 
    
    else if (cmd == OSPRDIOCRELEASE) 
    {
		// EXERCISE: Unlock the ramdisk.
		//
		// If the file hasn't locked the ramdisk, return -EINVAL.
		// Otherwise, clear the lock from filp->f_flags, wake up
		// the wait queue, perform any additional accounting steps
		// you need, and return 0.

		// Your code here (instead of the next line).
		//r = -ENOTTY;
        
        osp_spin_lock(&d->mutex);
        //could also just check if filp->flag is locked
        if (!d->ramdisk_WriteLocked && d->num_ReadLocks==0)
        {
            osp_spin_unlock(&d->mutex);
            return -EINVAL;
        }
        
        
        if (filp_writable)
        {
            d->ramdisk_WriteLocked=0;
            d->pid_holdingWriteLock=-1; //ensure no process holds write lock
			//eprintk("PID %d RELEASED WRITE LOCK\n", current->pid);
        }
        else //ramdisk was locked on reading
        {
			if (d->read_list != NULL)
			{
				//remove current pid from read_pid list
				if (d->read_list->reader == current->pid) // read lock in head
				{
					read_list_t del = d->read_list;
					d->read_list = d->read_list->next;
					kfree(del);
				}
				else // not in head (must be greater than 1 element)
				{
					read_list_t itr = d->read_list;
					while(itr->next != NULL && itr->next->reader != current->pid)
						itr = itr->next;
					
					if (itr->next != NULL)
					{
						read_list_t del = itr->next;
						itr->next = itr->next->next;
						kfree(del);
					}
				}
				d->num_ReadLocks--;
				//eprintk("PID %d RELEASED READ LOCK\n", current->pid);
			}
        }

        //chnage filp->flag to unlocked
		wake_up_all(&d->blockq);
        filp->f_flags &= !F_OSPRD_LOCKED;
        osp_spin_unlock(&d->mutex);
                
	} 
    else
		r = -ENOTTY; /* unknown command */
	return r;
}


// Initialize internal fields for an osprd_info_t.

static void osprd_setup(osprd_info_t *d)
{
	/* Initialize the wait queue. */
	init_waitqueue_head(&d->blockq);
	osp_spin_lock_init(&d->mutex);
	d->ticket_head = d->ticket_tail = 0;
	/* Add code here if you add fields to osprd_info_t. */
    d->ramdisk_WriteLocked=0;
    d->num_ReadLocks=0;
    d->pid_holdingWriteLock=-1;
    //add linked list part
	d->read_list = NULL;
    d->dead_tix = NULL;
}


/*****************************************************************************/
/*         THERE IS NO NEED TO UNDERSTAND ANY CODE BELOW THIS LINE!          */
/*                                                                           */
/*****************************************************************************/

// Process a list of requests for a osprd_info_t.
// Calls osprd_process_request for each element of the queue.

static void osprd_process_request_queue(request_queue_t *q)
{
	osprd_info_t *d = (osprd_info_t *) q->queuedata;
	struct request *req;

	while ((req = elv_next_request(q)) != NULL)
		osprd_process_request(d, req);
}


// Some particularly horrible stuff to get around some Linux issues:
// the Linux block device interface doesn't let a block device find out
// which file has been closed.  We need this information.

static struct file_operations osprd_blk_fops;
static int (*blkdev_release)(struct inode *, struct file *);

static int _osprd_release(struct inode *inode, struct file *filp)
{
	if (file2osprd(filp))
		osprd_close_last(inode, filp);
	return (*blkdev_release)(inode, filp);
}

static int _osprd_open(struct inode *inode, struct file *filp)
{
	if (!osprd_blk_fops.open) {
		memcpy(&osprd_blk_fops, filp->f_op, sizeof(osprd_blk_fops));
		blkdev_release = osprd_blk_fops.release;
		osprd_blk_fops.release = _osprd_release;
	}
	filp->f_op = &osprd_blk_fops;
	return osprd_open(inode, filp);
}


// The device operations structure.

static struct block_device_operations osprd_ops = {
	.owner = THIS_MODULE,
	.open = _osprd_open,
	// .release = osprd_release, // we must call our own release
	.ioctl = osprd_ioctl
};


// Given an open file, check whether that file corresponds to an OSP ramdisk.
// If so, return a pointer to the ramdisk's osprd_info_t.
// If not, return NULL.

static osprd_info_t *file2osprd(struct file *filp)
{
	if (filp) {
		struct inode *ino = filp->f_dentry->d_inode;
		if (ino->i_bdev
		    && ino->i_bdev->bd_disk
		    && ino->i_bdev->bd_disk->major == OSPRD_MAJOR
		    && ino->i_bdev->bd_disk->fops == &osprd_ops)
			return (osprd_info_t *) ino->i_bdev->bd_disk->private_data;
	}
	return NULL;
}


// Call the function 'callback' with data 'user_data' for each of 'task's
// open files.

static void for_each_open_file(struct task_struct *task,
		  void (*callback)(struct file *filp, osprd_info_t *user_data),
		  osprd_info_t *user_data)
{
	int fd;
	task_lock(task);
	spin_lock(&task->files->file_lock);
	{
#if LINUX_VERSION_CODE <= KERNEL_VERSION(2, 6, 13)
		struct files_struct *f = task->files;
#else
		struct fdtable *f = task->files->fdt;
#endif
		for (fd = 0; fd < f->max_fds; fd++)
			if (f->fd[fd])
				(*callback)(f->fd[fd], user_data);
	}
	spin_unlock(&task->files->file_lock);
	task_unlock(task);
}


// Destroy a osprd_info_t.

static void cleanup_device(osprd_info_t *d)
{
	wake_up_all(&d->blockq);
	if (d->gd) {
		del_gendisk(d->gd);
		put_disk(d->gd);
	}
	if (d->queue)
		blk_cleanup_queue(d->queue);
	if (d->data)
		vfree(d->data);
}


// Initialize a osprd_info_t.

static int setup_device(osprd_info_t *d, int which)
{
	memset(d, 0, sizeof(osprd_info_t));

	/* Get memory to store the actual block data. */
	if (!(d->data = vmalloc(nsectors * SECTOR_SIZE)))
		return -1;
	memset(d->data, 0, nsectors * SECTOR_SIZE);

	/* Set up the I/O queue. */
	spin_lock_init(&d->qlock);
	if (!(d->queue = blk_init_queue(osprd_process_request_queue, &d->qlock)))
		return -1;
	blk_queue_hardsect_size(d->queue, SECTOR_SIZE);
	d->queue->queuedata = d;

	/* The gendisk structure. */
	if (!(d->gd = alloc_disk(1)))
		return -1;
	d->gd->major = OSPRD_MAJOR;
	d->gd->first_minor = which;
	d->gd->fops = &osprd_ops;
	d->gd->queue = d->queue;
	d->gd->private_data = d;
	snprintf(d->gd->disk_name, 32, "osprd%c", which + 'a');
	set_capacity(d->gd, nsectors);
	add_disk(d->gd);

	/* Call the setup function. */
	osprd_setup(d);

	return 0;
}

static void osprd_exit(void);


// The kernel calls this function when the module is loaded.
// It initializes the 4 osprd block devices.

static int __init osprd_init(void)
{
	int i, r;

	// shut up the compiler
	(void) for_each_open_file;
#ifndef osp_spin_lock
	(void) osp_spin_lock;
	(void) osp_spin_unlock;
#endif

	/* Register the block device name. */
	if (register_blkdev(OSPRD_MAJOR, "osprd") < 0) {
		printk(KERN_WARNING "osprd: unable to get major number\n");
		return -EBUSY;
	}

	/* Initialize the device structures. */
	for (i = r = 0; i < NOSPRD; i++)
		if (setup_device(&osprds[i], i) < 0)
			r = -EINVAL;

	if (r < 0) {
		printk(KERN_EMERG "osprd: can't set up device structures\n");
		osprd_exit();
		return -EBUSY;
	} else
		return 0;
}


// The kernel calls this function to unload the osprd module.
// It destroys the osprd devices.

static void osprd_exit(void)
{
	int i;
	for (i = 0; i < NOSPRD; i++)
		cleanup_device(&osprds[i]);
	unregister_blkdev(OSPRD_MAJOR, "osprd");
}


// Tell Linux to call those functions at init and exit time.
module_init(osprd_init);
module_exit(osprd_exit);
