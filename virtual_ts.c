#include <linux/input.h>
#include <linux/input/mt.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/uaccess.h>
#include <asm/io.h>
#include <asm/delay.h>
#include <asm/uaccess.h>

#define MODNAME      "virtual_ts"
#define COMMANDS_MAX 1024
#define COMMAND_MAX  64
#define MAX_CONTACTS 10 // max count of supported fingers
#define PRESSURE     19
#define TOUCH_MAJOR  10

struct touch {
	int x;
	int y;
	int touched;
};

static struct touch touch_ids[MAX_CONTACTS];

static int device_open(struct inode *, struct file *);
static int device_release(struct inode *, struct file *);
static ssize_t device_read(struct file *, char *, size_t, loff_t *);
static ssize_t device_write(struct file *, const char *, size_t, loff_t *);
static char *rtrim(char *str,const char *seps);

static int Major;	         /* Major number assigned to our device driver */
static int Device_Open = 0;  /* Is device open?  Used to prevent multiple access to the device */

static struct class * cl;
static struct device * dev; 

struct file_operations fops = {
	read: device_read,
	write: device_write,
	open: device_open,
	release: device_release
};

static struct input_dev *virt_ts_dev;

static int ABS_X_MAX = 0;
module_param(ABS_X_MAX,int,0);
MODULE_PARM_DESC(ABS_X_MAX,"Max width of your display. It might be used in case of fb setting does not work.");

static int ABS_Y_MAX = 0;
module_param(ABS_Y_MAX,int,0);
MODULE_PARM_DESC(ABS_Y_MAX,"Max height of your display. It might be used in case of fb setting does not work.");

static char * RESOLUTION = "";
module_param(RESOLUTION,charp,0);
MODULE_PARM_DESC(RESOLUTION,"String representation of ABS_X_MAXxABS_Y_MAX.");

static int replacechar(char *str,char orig,char rep) {
	char *ix = str;
	int n = 0;
	while((ix = strchr(ix, orig)) != NULL) {
		*ix++ = rep;
		n++;
	}
	return n;
}

static int display_setup(void) {
	if (ABS_X_MAX <= 0 || ABS_Y_MAX <= 0) {
		if (!strcmp(RESOLUTION,"")) {
			printk("<4>virtual_ts: ABS_X_MAX and ABS_Y_MAX or RESOLUTION module parms need to be set!\n");
			return 0;
		}
		replacechar(RESOLUTION,'x',' ');
		if(sscanf(RESOLUTION,"%d%d",&ABS_X_MAX,&ABS_Y_MAX) != 2) {
			printk("<4>virtual_ts: cannot parse the contents of %s\n",RESOLUTION);
			return 0;
		}
	}
	printk("virtual_ts: ABS_X_MAX=%d, ABS_Y_MAX=%d\n",ABS_X_MAX,ABS_Y_MAX);
    
	return 1;
}

static int __init virt_ts_init(void) {
	int err;
	int i;
    
	if (!display_setup()) return -1;
    
	for(i=0;i<MAX_CONTACTS;i++) {
		touch_ids[i].x = 0;
		touch_ids[i].y = 0;
		touch_ids[i].touched = 0;
	}
    
	virt_ts_dev = input_allocate_device();
	if (!virt_ts_dev)
		return -ENOMEM;

	__set_bit(EV_ABS,virt_ts_dev->evbit);
	__set_bit(EV_SYN,virt_ts_dev->evbit);

	virt_ts_dev->name = "virtual_touchscreen";
	virt_ts_dev->phys = "virtual_ts/input0";
	virt_ts_dev->id.bustype = BUS_HOST;
	virt_ts_dev->id.vendor = 0x9999;
	virt_ts_dev->id.product = 0x9999;
	virt_ts_dev->id.version = 1;

	input_set_abs_params(virt_ts_dev,ABS_MT_PRESSURE,0,PRESSURE,0,0);
	input_set_abs_params(virt_ts_dev,ABS_MT_TOUCH_MAJOR,0,TOUCH_MAJOR,0,0);
	input_set_abs_params(virt_ts_dev,ABS_MT_POSITION_X,0,ABS_X_MAX,0,0);
	input_set_abs_params(virt_ts_dev,ABS_MT_POSITION_Y,0,ABS_Y_MAX,0,0);
	input_mt_init_slots(virt_ts_dev,MAX_CONTACTS,0);
	input_set_abs_params(virt_ts_dev,ABS_MT_TRACKING_ID,0,MAX_CONTACTS - 1,0,0);
	__set_bit(INPUT_PROP_DIRECT,virt_ts_dev->propbit);    
    
	err = input_register_device(virt_ts_dev);
	if (err) {
		printk("<4>virtual_ts: Registering the input device failed\n");
		input_free_device(virt_ts_dev);
		return err;
	}

	/* Above is evdev part. Below is character device part */

	Major = register_chrdev(0, MODNAME, &fops);	
	if (Major < 0) {
		printk("<4>virtual_ts: Registering the character device failed\n");
		input_free_device(virt_ts_dev);
		return Major;
	}
	printk("virtual_ts: Major=%d\n", Major);

	cl = class_create(THIS_MODULE, MODNAME);
	if (!IS_ERR(cl)) dev = device_create(cl, NULL, MKDEV(Major,0), NULL, MODNAME);

	return 0;
}

static int device_open(struct inode *inode, struct file *file) {
	if (Device_Open) return -EBUSY;
	++Device_Open;
	return 0;
}
	
static int device_release(struct inode *inode, struct file *file) {
	--Device_Open;
	return 0;
}
	
static ssize_t device_read(struct file *filp, char *buffer, size_t length, loff_t *offset) {
	const char* message = 
	"Usage: write the following commands to /dev/virtual_ts:\n"
	"   TOUCH   x y'\\n'           - create new touch in (x,y)\n"  
	"   MOVE    touch_id x y'\\n'  - move touch with ID=touch_id to (x,y)\n"
	"                                (x or y can be -1 if moving only in one direction is necessary)\n"  
	"   UNTOUCH touch_id'\\n'      - untouch ID=touch_id\n" 
	"\n"
	"  Associated ID of the touch is always the first free one and starts from 0\n"
	"  Driver does not return IDs it associates so you need to keep them in mind to have your code workable!\n"
	"  Extra touches are being ommited and corresponding error is being generated!\n"        
	"  Moreover, if you register unusual behaviour then use dmesg to find out what's wrong!\n";      
	const size_t msgsize = strlen(message);
	loff_t off = *offset;
	if (off >= msgsize) return 0;
	if (length > msgsize - off) length = msgsize - off;
	if (copy_to_user(buffer, message+off, length) != 0) return -EFAULT;

	*offset+=length;
	return length;
}

static char * ltrim(char *str,const char * seps) {
	size_t totrim;
	
	if (seps == NULL) seps = "\t ";
	totrim = strspn(str,seps);
	if (totrim > 0) {
		size_t len = strlen(str);
		if (totrim == len)  str[0] = '\0';
		else memmove(str, str + totrim, len + 1 - totrim);
	}
	return str;
}

static char *rtrim(char *str,const char *seps) {
	int i;
	if (seps == NULL) seps = "\t ";
	i = strlen(str) - 1;
	while (i >= 0 && strchr(seps, str[i]) != NULL) {
		str[i] = '\0';
		i--;
	}
	return str;
}

static int find_free_slot(void) {
	int i;
    
	for (i=0;i<MAX_CONTACTS;i++) {
		if (!touch_ids[i].touched) return i;
	}
    
	return -1;
}

static void send_commands(int slot) {
	input_mt_slot(virt_ts_dev,slot);
	if (touch_ids[slot].touched) {
		input_report_abs(virt_ts_dev,ABS_MT_TRACKING_ID,slot);
		input_report_abs(virt_ts_dev,ABS_MT_POSITION_X,touch_ids[slot].x);
		input_report_abs(virt_ts_dev,ABS_MT_POSITION_Y,touch_ids[slot].y);
		input_report_abs(virt_ts_dev,ABS_MT_TOUCH_MAJOR,TOUCH_MAJOR);            
		input_report_abs(virt_ts_dev,ABS_MT_PRESSURE,PRESSURE);
	}
	else {
		input_report_abs(virt_ts_dev,ABS_MT_TRACKING_ID,-1);
	}
	input_sync(virt_ts_dev);
}

static void fix_coords(int * x,int * y,int def_x,int def_y) {
	if (*x < 0) *x = def_x;
	if (*y < 0) *y = def_y;
	if (*x > ABS_X_MAX) *x = ABS_X_MAX;
	if (*y > ABS_Y_MAX) *y = ABS_X_MAX; 
}

static int execute_command(char * cmd) {
	char command[COMMAND_MAX];
	char args[COMMAND_MAX];
	int  iarg1;
	int  iarg2;
	int  slot;
	char * res;

	ltrim(cmd,NULL);
	rtrim(cmd,NULL);
        
	if (strlen(cmd) >= COMMAND_MAX) return 5;

	res = strchr(cmd,' ');
	if (res != NULL) {
		memcpy(command,cmd,res-cmd);
		command[res-cmd] = '\0';
	}
	else strcpy(command,cmd);
	strcpy(args,(res == NULL)?"":res + 1);
    
	if (!strcmp(command,"TOUCH")) {
		if(sscanf(args,"%d%d",&iarg1,&iarg2) != 2) return 1;
		slot = find_free_slot();
		if (slot == -1) return 3;
		fix_coords(&iarg1,&iarg2,0,0);
        
		touch_ids[slot].x = iarg1;
		touch_ids[slot].y = iarg2;
		touch_ids[slot].touched = 1;
	}
	else if (!strcmp(command,"UNTOUCH")) {
		if(sscanf(args,"%d",&slot) != 1) return 1;
		if ((slot < 0) || (slot >= MAX_CONTACTS)) return 1;
		if (!touch_ids[slot].touched) return 4;
        
		touch_ids[slot].x = 0;
		touch_ids[slot].y = 0;
		touch_ids[slot].touched = 0;
	}
	else if (!strcmp(command,"MOVE")) {
		if(sscanf(args,"%d%d%d",&slot,&iarg1,&iarg2) != 3) return 1;
		if ((slot < 0) || (slot >= MAX_CONTACTS)) return 1;
		if (!touch_ids[slot].touched) return 4;
		fix_coords(&iarg1,&iarg2,touch_ids[slot].x,touch_ids[slot].y);    
        
		touch_ids[slot].x = iarg1;
		touch_ids[slot].y = iarg2;
	}
	else return 2;
      
	send_commands(slot);
    
	return 0;
}

static ssize_t device_write(struct file *filp, const char *buff, size_t len, loff_t *off) {
	char buf[COMMAND_MAX];
	size_t i;
	size_t p=0;
	int ret = 1;

	if (len == 0) return 0;
	if (len > COMMAND_MAX) {
		printk("<4>virtual_ts: line of commands is too long. Trailing \\n is required.\n");
		return len;
	}
        
	if (copy_from_user(buf, buff, len) != 0) return -EFAULT;
    
	for(i=0; i<len; ++i) {
		if (buf[i]=='\n') {
			buf[i] = '\0';
			if ((ret = execute_command(buf+p))) {
				switch (ret) {
					case 1:
						printk("<4>virtual_ts: command's interpretation was wrong: %s\n",buf+p);
						break;
					case 2:
						printk("<4>virtual_ts: unknown command found: %s\n",buf+p);
						break;
					case 3:
						printk("<4>virtual_ts: too many touches identified: %s\n",buf+p);
						break;                        
					case 4:
						printk("<4>virtual_ts: touch with inputed id does not exist: %s\n",buf+p);
						break;                        
					case 5:
						printk("<4>virtual_ts: command is too long: %s\n",buf+p);
						break;                        
				}
			}
			p=i+1;
		}
	}
	
	return p;
}

static void __exit virt_ts_exit(void) {
	input_mt_destroy_slots(virt_ts_dev);
	input_unregister_device(virt_ts_dev);

	if (!IS_ERR(cl)) {
		device_destroy(cl, MKDEV(Major,0));
		class_destroy(cl);
	}
	unregister_chrdev(Major, MODNAME);
}

module_init(virt_ts_init);
module_exit(virt_ts_exit);

MODULE_AUTHOR("Initial author: Vitaly Shukela, vi0oss@gmail.com\nModified by Alex Levkovich, alevkovich@tut.by");
MODULE_DESCRIPTION("Virtual touchscreen driver");
MODULE_LICENSE("GPL");

