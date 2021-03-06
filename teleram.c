#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/device.h>
#include <linux/init.h> 
#include <linux/fs.h> 
#include <linux/mm.h> 
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/sched.h>
#include <linux/net.h>
#include <linux/in.h>
#include <linux/netpoll.h>
#include <asm/pgtable_types.h>
#include <linux/string.h>

#define MAX_SIZE (PAGE_SIZE)   /* max size mmaped to userspace */
#define DEVICE_NAME "tram"
#define  CLASS_NAME "mogu"
#define MESSAGE_SIZE 4096
#define MAX_BUFS 4 //https://lwn.net/Articles/510202/ should make this a hashtable
#define INADDR_SEND ((unsigned long int) (0x7f000001)) //127.0.0.1

static struct socket *sock;
static struct socket *sock_send;
static struct sockaddr_in sin;
static struct sockaddr_in sin_send;
static struct task_struct *thread;
//static struct page *page;
static int error, len;
static char message[MESSAGE_SIZE];
static struct class*  class;
static struct device*  device;
static int major;
static int num_buffers = 1; //default support for 1 page
static char *sh_mem = NULL;
static char *ip;
static long unsigned int ip_long = 0x00000000;
static unsigned int port;
static char** buffers[MAX_BUFS];
//static phys_addr_t sh_mem_addr;

static DEFINE_MUTEX(mchar_mutex);


/* internal buffer mapping */
struct vm_area_struct *sh_mem_vma = NULL;

pte_t *lookup_address_in_pgd(pgd_t *pgd, unsigned long address,
                 unsigned int *level)
{
    p4d_t *p4d;
    pud_t *pud;
    pmd_t *pmd;

    *level = PG_LEVEL_NONE;

    if (pgd_none(*pgd))
        return NULL;

    p4d = p4d_offset(pgd, address);
    if (p4d_none(*p4d)) {
        pr_info("failed 1");
                return NULL;

    }


    *level = PG_LEVEL_512G;
    if (p4d_large(*p4d) || !p4d_present(*p4d))
        return (pte_t *)p4d;

    pud = pud_offset(p4d, address);
    if (pud_none(*pud))
        {
        pr_info("failed 2");
                return NULL;

    }

    *level = PG_LEVEL_1G;
    if (pud_large(*pud) || !pud_present(*pud))
        return (pte_t *)pud;

    pmd = pmd_offset(pud, address);
    if (pmd_none(*pmd))
        {
        pr_info("failed 3");
                return NULL;

    }

    *level = PG_LEVEL_2M;
    if (pmd_large(*pmd) || !pmd_present(*pmd))
        return (pte_t *)pmd;

    *level = PG_LEVEL_4K;

    return pte_offset_kernel(pmd, address);
}

/*  executed once the device is closed or releaseed by userspace
 *  @param inodep: pointer to struct inode
 *  @param filep: pointer to struct file 
 */
static int mchar_release(struct inode *inodep, struct file *filep)
{    
    mutex_unlock(&mchar_mutex);
    pr_info("mchar: Device successfully closed\n");

    return 0;
}

/* executed once the device is opened.
 *
 */
static int mchar_open(struct inode *inodep, struct file *filep)
{
    int ret = 0; 
    if(!mutex_trylock(&mchar_mutex)) {
        pr_alert("mchar: device busy!\n");
        ret = -EBUSY;
        goto out;
    }
    
    pr_info("mchar: Device opened\n");

out:
    return ret;
}

/*
creates two sockets: listening socket and sending socket
*/
static int udp_socket_setup(void) {
    if ((error = sock_create(AF_INET, SOCK_DGRAM, IPPROTO_UDP, &sock)) < 0 ||
        (error = sock_create(AF_INET, SOCK_DGRAM, IPPROTO_UDP, &sock_send)) < 0) {
        pr_info("failed to created udp sock %d", error);
        goto out;
    }

    /*set up network addressing*/
    sin_send.sin_family = AF_INET;
    sin.sin_family = AF_INET;
    sin_send.sin_port = htons(1337); //fix this
    sin.sin_port = htons(1337); //fix this

    sin_send.sin_addr.s_addr = htonl(ip_long);
    sin.sin_addr.s_addr = htonl(INADDR_ANY);

    if ((error = sock->ops->bind(sock, (struct sockaddr *)&sin, sizeof(struct sockaddr)) < 0)) {
        pr_info("failed to bind udp socket %d", error);
        goto out;
    }
    //other socket doesn't even need to connect



    out:
        return error;
}

//probably will change to tcp and move to separate kthread
static int receive_udp_msg(char *buf, int len) {
    struct msghdr msg;
    //struct iovec iov;
    struct kvec kv;

    memset(&msg, 0, sizeof(msg));
    //memset(&iov, 0, sizeof(iov));
    memset(&kv, 0, sizeof(kv));
    //int size = 0;

    if (sock->sk==NULL) 
        return 0;
    //talk t d
    //can do separate kernel thread w MSG_WAITALL and block OR
    //can wing it and be a bit more efficient w timeout and MSG_DONTWAIT
    //will have to send sig_stop or something to put it to sleep so it doesn't busy spin->sig_cont
    msg.msg_flags = MSG_WAITALL /*MSG_DONTWAIT*/;
    msg.msg_name = &sin;
    msg.msg_namelen  = sizeof(struct sockaddr_in);
    msg.msg_control = NULL;
    msg.msg_controllen = 0;
    //iov.iov_base = buf;
    //iov.iov_len = len;

    kv.iov_base = buf;
    kv.iov_len = len;

    //iov_iter_init(&msg.msg_iter, WRITE, &iov, 1, len);
    //old_fs = get_fs();
    //set_fs(KERNEL_DS);
    //pr_info("waiting for msg in receive_udp_msg");
    //receive exactly 1 msg from subsystem. what to do when multiple??
    error = kernel_recvmsg(sock,&msg, &kv,1, len, msg.msg_flags);
    //set_fs(old_fs);
    if (error > 1) {
        pr_info("received bytes: %s, %d", buf, error);
    }
    return error;
}

static void send_dank_msg(void) {
    struct msghdr msg;
    struct kvec kv;

    memset(&msg, 0, sizeof(msg));
    memset(&kv, 0, sizeof(kv));
    sprintf(message, "there is no documentation anywhere");
    len = strlen(message);
    msg.msg_name = &sin_send;
    msg.msg_namelen  = sizeof(struct sockaddr_in);
    kv.iov_base = message;
    kv.iov_len = len;
    error = kernel_sendmsg(sock_send,&msg,&kv,1,len);
    //pr_info("sent bytes: %d", error); //errors are <0, # bytes sent are >0
}

void simple_vma_open(struct vm_area_struct *vma)
{
    printk(KERN_NOTICE "VMA open ======================================="); /*, virt %lx, phys %lx\n",
    vma->vm_start, vma->vm_pgoff << PAGE_SHIFT);*/
}
void simple_vma_close(struct vm_area_struct *vma)
{
    printk(KERN_NOTICE "VMA close.\n");
}


vm_fault_t simple_vma_fault(struct vm_fault *vmf)
{
    /*
    todo
    -remapping old vma handled by user library
    -page out current: send network message w/ k=sh_mem_vma_start, v=page contents(sh_mem)
    -page in new address: send network msg request k=vmf->vma_start, v=page contents
    kill_pid(task_pid(thread->pid), SIGSTOP, 1)
    if multiple programs mmap simultaneously, unfortunately only one will get it, so should map a page for each program?
    */
    //vma is vm area associated with a particular handler/allocated by mmap?
    pr_info("fault caused by pid: %d for addr: %lx", current->pid, vmf->address);
    // unmap_pte or in userspace lib call munmap manually and then remap
    struct vm_area_struct *vma = vmf->vma;    
    




    long unsigned int offset;
    offset = (((long unsigned int)vmf->address - vma->vm_start) + (vma->vm_pgoff << PAGE_SHIFT));
    if (offset > PAGE_SIZE << 4) {
        goto nopage_out;  
    }
    //*/
    pr_info("simple_fault: sh_mem: %s", sh_mem);
    struct page* page = virt_to_page(sh_mem + offset); //why does 2 pages not cause issues?
    get_page(page); //refcount, update page metadata
    vmf->page = page;

    sh_mem_vma = vmf->vma;
    //need to remove all sh_mem page mappings in the previous vma first
    //then need to map in current vma sh_mem page.
    pr_info("simple_vma_fault: finished vm fault op");
    send_dank_msg();
        //pr_info("%llx", (long long unsigned int)(*((long long unsigned int *)0xf00000000000321c8)));

nopage_out:
    return 0;
}

static struct vm_operations_struct simple_remap_vm_ops = {
.open = simple_vma_open,
.close = simple_vma_close,
.fault = simple_vma_fault,
};
static int mchar_mmap(struct file *filp, struct vm_area_struct *vma)
{
    //printk(KERN_INFO "Device simple_vma_ops_mmap\n");
    //vma->vm_private_data = sh_mem;
    vma->vm_ops = &simple_remap_vm_ops;
    simple_vma_open(vma);
    printk(KERN_INFO "Device mmap OK\n");
    return 0;
}



static const struct file_operations mchar_fops = {
    .open = mchar_open,
    //.read = mchar_read,
    //.write = mchar_write,
    .release = mchar_release,
    .mmap = mchar_mmap,
    /*.unlocked_ioctl = mchar_ioctl,*/
    .owner = THIS_MODULE,
};

static char *devnode(struct device *dev, umode_t *mode)
{
    printk("setting permissions for device");
    if (!mode)
        return NULL;
    *mode = 0666;
    return NULL;
}


static int ksocket_start(void) {
    allow_signal(SIGKILL);
    for (;;)
    {
        memset(sh_mem, 0, MESSAGE_SIZE); //kthreads share the same address space so no need to map
        if (signal_pending(current))
            break;
        //pr_info("blocking on receive in thread");
        receive_udp_msg(sh_mem, 34);
        //pr_info("thread: sh_mem: %s", sh_mem);
        
    }
    pr_info("thread attempted to exit");
    do_exit(1);
    return 0;
}
static int lthread_start(void) {
    thread = kthread_run((void *)ksocket_start, NULL, "test");
    if (IS_ERR(thread)) 
    {
        printk(KERN_INFO "test: unable to start kernel thread\n");
        return -ENOMEM;
    }
    //pr_info("thread started with pid: %d", thread->pid);
    return 0;

}

static int __init mchar_init(void)
{
    int ret = 0;

    /*register device and class */  
    major = register_chrdev(0, DEVICE_NAME, &mchar_fops);

    if (major < 0) {
        pr_info("failed to register major");
        ret = major;
        goto out;
    }

    class = class_create(THIS_MODULE, CLASS_NAME);
    if (IS_ERR(class)){ 
        unregister_chrdev(major, DEVICE_NAME);
        pr_info("failed to register device class");
        ret = PTR_ERR(class);
        goto out;
    }

    /*set device class permissions*/
    class->devnode=devnode;

    /*create device*/
    device = device_create(class, NULL, MKDEV(major, 0), NULL, DEVICE_NAME);
    if (IS_ERR(device)) {
        class_destroy(class);
        unregister_chrdev(major, DEVICE_NAME);
        ret = PTR_ERR(device);
        goto out;
    }

    /*how many internal buffers to maintain (1 per program that requests mem)*/
    if (ip != NULL) {
        char *tmp = ip;
        int i = 3;
        while (i >= 0) {
            char* tstart = tmp;
            while (*tmp != '.') {
                tmp++;
            }
            *tmp = '\0';
            tmp++;
            unsigned long part = simple_strtoul(tstart,&tstart, 10);
            ip_long |= part << (8 * (i));
            i--;
        }
        pr_info("ip: %lx", ip_long);
    } else {
        ip_long = INADDR_SEND;
    }
    

    /*allocate internal shared buffer*/
    //TODO: add hashmap with k:pid, v:buffer contents?
    //TODO define network api so that received msgs know which buffers to pop
    int i = 0;
    for (i = 0; i < num_buffers && i < MAX_BUFS; i++) {
        buffers[i] = kmalloc(4096 * 2 + 1, GFP_KERNEL);
        if (buffers[i] == NULL)
        {
            pr_info("failed to alloc sh_mem");
            ret = -ENOMEM; 
            goto out;
        }
    }
    sh_mem = kmalloc(4096 * 2 + 1, GFP_KERNEL); //weird race condition temp workaround :/
    

    /*create udp network socket*/
    if (udp_socket_setup() < 0) {
        pr_info("failed to setup sockets, exiting");
        do_exit(1);
    }

    mutex_init(&mchar_mutex);
    /*debug*/
    printk("init");
    sprintf(sh_mem, "xyz\n"); 

    lthread_start();

    send_dank_msg();
    send_dank_msg();

    pr_info("module pid :%d, thread pid: %d", current->pid, thread->pid);
    pr_info("finished set up");
    msleep(300);
out: 
    return 0;
}

static void __exit mchar_exit(void)
{
    pr_info("exit called");
    pr_info("attempting to stop thread");
    if (thread != NULL) {
        kill_pid(find_vpid(thread->pid), SIGKILL, 1);
        //thread = NULL;
        kthread_stop(thread);//will this thread reference be accurate here?
        
    }
    pr_info("attempted to stop thread");

    mutex_destroy(&mchar_mutex);
    device_destroy(class, MKDEV(major, 0));  
    class_unregister(class);
    class_destroy(class); 
    unregister_chrdev(major, DEVICE_NAME);
    kfree(sh_mem);
    int i = 0;
    for (i = 0; i < num_buffers && i < MAX_BUFS; i++) {
        kfree(buffers[i]);
    }

    
    
    sock_release(sock);
    sock_release(sock_send);
    pr_info("mchar: unregistered!");
}

module_init(mchar_init);
module_exit(mchar_exit);
module_param(num_buffers, int, 0644);
module_param(ip, charp, 0644);
module_param(port, int, 0644);
MODULE_LICENSE("GPL");