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
#define MESSAGE_SIZE 4096
#define INADDR_SEND ((unsigned long int) (0x7f000001)) //127.0.0.1
static struct socket *sock;
static struct socket *sock_send;
static struct sockaddr_in sin;
static struct sockaddr_in sin_send;
static struct task_struct *thread;


static int error, len;
static mm_segment_t old_fs; //setfs ->used for switching btw accessing kernel mem and user mem
static char message[MESSAGE_SIZE];

#define MAX_SIZE (PAGE_SIZE)   /* max size mmaped to userspace */
#define DEVICE_NAME "tram"
#define  CLASS_NAME "mogu"

static struct class*  class;
static struct device*  device;
static int major;
static char *sh_mem = NULL;
static phys_addr_t sh_mem_addr;

static DEFINE_MUTEX(mchar_mutex);

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

    sin_send.sin_addr.s_addr = htonl(INADDR_SEND);
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

    msg.msg_flags = MSG_WAITALL;
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
    pr_info("waiting for msg in receive_udp_msg");
    error = kernel_recvmsg(sock,&msg, &kv,1, len, msg.msg_flags);
    //set_fs(old_fs);
    pr_info("received bytes: %s, %d", buf, error);
    return error;
}

static void send_dank_msg(void) {
   
    struct msghdr msg;
    //struct iovec iov;
    struct kvec kv;
    //kmap(NULL);

    memset(&msg, 0, sizeof(msg));
    //memset(&iov, 0, sizeof(iov));
    memset(&kv, 0, sizeof(kv));

    pr_info("send_dank_msg: message buffer: %p", message);
    sprintf(message, "there is no documentation anywhere");
    len = strlen(message);
    //msg.msg_flags = 0;
    //msg.msg_iocb = NULL;
    msg.msg_name = &sin_send;
    msg.msg_namelen  = sizeof(struct sockaddr_in);
    //msg.msg_control = NULL;
    //msg.msg_controllen = 0;
    //iov.iov_base = message;
    //iov.iov_len = len;

    kv.iov_base = message;
    kv.iov_len = len;


    //iov_iter_init(&msg.msg_iter, WRITE, &iov, 1, len); //last arg is length??
    //old_fs = get_fs();
    //set_fs(KERNEL_DS);
    pr_info("send_dank_msg: preparing to send msg");
    error = kernel_sendmsg(sock_send,&msg,&kv,1,len);
    //set_fs(old_fs);
    pr_info("sent bytes: %d", error); //errors are <0, # bytes sent are >0
}

void simple_vma_open(struct vm_area_struct *vma)
{
    printk(KERN_NOTICE "VMA open, virt %lx, phys %lx\n",
    vma->vm_start, vma->vm_pgoff << PAGE_SHIFT);
}
void simple_vma_close(struct vm_area_struct *vma)
{
    printk(KERN_NOTICE "VMA close.\n");
}


unsigned int simple_vma_fault(struct vm_fault *vmf)
{

    struct vm_area_struct *vma = vmf->vma;
    struct page *page = NULL;
    unsigned long offset;
    printk(KERN_NOTICE "MWR: simple_vma_fault\n");
    offset = (((unsigned long)vmf->address - vma->vm_start) + (vma->vm_pgoff << PAGE_SHIFT));
    if (offset > PAGE_SIZE << 4) {
        goto nopage_out;  
    }
    printk("virt to page? %lx", vmf->address);
    page = virt_to_page((unsigned long)sh_mem + offset);
    pr_info("page addr: %p", page);
    get_page(page); //refcount, update page metadata
    vmf->page = page;
    printk("vm:%p ",sh_mem);


    send_dank_msg();

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
    printk(KERN_INFO "Device simple_vma_ops_mmap\n");
    vma->vm_private_data = sh_mem;
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
    //current->flags |= PF_NOFREEZE;
    allow_signal(SIGKILL);
    int size;

    

    for (;;)
    {
        memset(sh_mem, 0, MESSAGE_SIZE); //kthreads share the same address space so no need to map
        if (signal_pending(current))
            break;
        pr_info("blocking on receive in thread");
        size = receive_udp_msg(sh_mem, 34);
        pr_info("thread: sh_mem: %s", sh_mem);
        
    }
    pr_info("thread attempted to exit");
    do_exit(1);
    return 0;
}
static int lthread_start(void) {

    //sh_mem_addr = virt_to_phys(sh_mem);

    //pr_info("sh_mem_phys: %llu", sh_mem_addr);
    thread = kthread_run((void *)ksocket_start, NULL, "test");
    if (IS_ERR(thread)) 
    {
        printk(KERN_INFO "test: unable to start kernel thread\n");
        return -ENOMEM;
    }
    pr_info("thread started with pid: %d", thread->pid);
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

    /*allocate internal shared buffer*/
    sh_mem = kzalloc(MAX_SIZE, GFP_KERNEL); 
    if (sh_mem == NULL) {
        ret = -ENOMEM; 
        goto out;
    }

    /*create udp network socket*/
    if (udp_socket_setup() < 0) {
        pr_info("failed to setup sockets, exiting");
        do_exit(1);
    }

    mutex_init(&mchar_mutex);
    /*debug*/
    printk("init");
    sprintf(sh_mem, "xyz\n"); 
    //struct page* page;
    //page = virt_to_page(sh_mem);
    //pr_info("device internal buffer: %p, page addr: %p", sh_mem, page);
    

    lthread_start();
    pr_info("preparing to send msg");
    //error = sock->ops->connect(sock, (struct sockaddr *)&sin, sizeof(struct sockaddr), 0);
    //receive_udp_msg(sh_mem, 34);
    send_dank_msg();
    send_dank_msg();
    pr_info("sh_mem: %s", sh_mem);

    pr_info("pid :%d", current->pid);
    pr_info("finished set up");
    msleep(300);
    pr_info("base: sh_mem: %s", sh_mem);

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

    
    
    sock_release(sock);
    sock_release(sock_send);
    pr_info("mchar: unregistered!");
}

module_init(mchar_init);
module_exit(mchar_exit);
MODULE_LICENSE("GPL");