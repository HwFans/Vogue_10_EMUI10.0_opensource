/*
 *  Hisilicon K3 SOC camera driver source file
 *
 *  Copyright (C) Huawei Technology Co., Ltd.
 *
 * Author:
 * Email:
 * Date:	  2013-10-29
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
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */


#include <linux/atomic.h>
#include <linux/fs.h>
#include <linux/debugfs.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/sched.h>
#include <linux/errno.h>
#include <linux/freezer.h>
#include <linux/pid.h>
#include <linux/slab.h>
#include <linux/videodev2.h>
#include <media/media-device.h>
#include <media/v4l2-dev.h>
#include <media/v4l2-device.h>
#include <media/v4l2-event.h>
#include <media/v4l2-fh.h>
#include <media/v4l2-ioctl.h>
#include <securec.h>

#include "hwcam_intf.h"
#include "cam_log.h"
#include "hwcam_compat32.h"
#include <dsm/dsm_pub.h>
#include <linux/of.h>
//lint -save -e455 -e429 -e454

#define CREATE_TRACE_POINTS
#include "trace_hwcam.h"

struct dsm_client_ops ops2={
	.poll_state = NULL,
	.dump_func = NULL,
};

struct dsm_dev dev_camera_user = {
	.name = "dsm_camera_user",
	.device_name = NULL,
	.ic_name = NULL,
	.module_name = NULL,
	.fops = &ops2,
	.buff_size = 4096,
};

struct dsm_client *client_camera_user;

typedef struct _tag_hwcam_cfgdev_vo
{
    struct v4l2_device                          v4l2;
    struct video_device*                        vdev;
    struct media_device*                        mdev;

    struct dentry*                              debug_root;
	struct v4l2_fh                              rq;
    __u8                                        sbuf[64];
    struct mutex                                lock;
} hwcam_cfgdev_vo_t;

typedef enum _tag_hwcam_cfgsvr_flags
{
    HWCAM_CFGSVR_FLAG_UNPLAYED = 0,
    HWCAM_CFGSVR_FLAG_PLAYING = 1,
} hwcam_cfgsvr_flags_t;

static DEFINE_MUTEX(s_cfgdev_lock);
static hwcam_cfgdev_vo_t s_cfgdev;

static DEFINE_MUTEX(s_cfgsvr_lock);
static DECLARE_WAIT_QUEUE_HEAD(s_wait_cfgsvr);
static struct pid* s_pid_cfgsvr;
static int is_binderized = 0; //default 0 is Passthrough

void
hwcam_cfgdev_lock(void)
{
    mutex_lock(&s_cfgdev_lock);
}

void
hwcam_cfgdev_unlock(void)
{
    mutex_unlock(&s_cfgdev_lock);
}

static DEFINE_SPINLOCK(s_ack_queue_lock);
static atomic_t s_sequence = ATOMIC_INIT(0);
static hwcam_cfgsvr_flags_t s_cfgsvr_flags = HWCAM_CFGSVR_FLAG_UNPLAYED;
static struct list_head s_ack_queue = LIST_HEAD_INIT(s_ack_queue);
static DECLARE_WAIT_QUEUE_HEAD(s_wait_ack);

static void hwcam_cfgdev_work(struct work_struct* w);
static DECLARE_DELAYED_WORK(s_cfgdev_work, hwcam_cfgdev_work);


static ssize_t guard_thermal_show(struct device_driver *drv, char *buf);
static ssize_t guard_thermal_store(struct device_driver *drv, const char *buf, size_t count);
static DRIVER_ATTR_RW(guard_thermal);

static ssize_t dump_meminfo_show(struct device_driver *drv, char *buf);
static ssize_t dump_meminfo_store(struct device_driver *drv, const char *buf, size_t count);
static DRIVER_ATTR_RW(dump_meminfo);

/*
 * set camera log level by device
 */
uint32_t hwcam_debug_mask = CAM_DEBUG_INFO |CAM_DEBUG_EMERG | CAM_DEBUG_ALERT | \
							CAM_DEBUG_CRIT | CAM_DEBUG_ERR| CAM_DEBUG_WARING;

#ifdef DEBUG_HISI_CAMERA
static ssize_t hwcam_log_show(struct device_driver *drv, char *buf);
static ssize_t hwcam_log_store(struct device_driver *drv, const char *buf, size_t count);
static DRIVER_ATTR_RW(hwcam_log);
#endif

static int
hwcam_cfgdev_guard_thermal(void)
{
    struct v4l2_event ev =
    {
        .type = HWCAM_V4L2_EVENT_TYPE,
        .id = HWCAM_CFGDEV_REQUEST,
    };
    hwcam_cfgreq2dev_t* req = (hwcam_cfgreq2dev_t*)ev.u.data;

    req->req.intf = NULL;
    req->kind = HWCAM_CFGDEV_REQ_GUARD_THERMAL;
    return hwcam_cfgdev_send_req(NULL, &ev, &s_cfgdev.rq, 1, NULL);
}

static ssize_t guard_thermal_store(struct device_driver *drv,
												  const char *buf, size_t count)
{
	int ret = 0;
	cam_info("%s enter", __func__);
	if (count > 1) {
	    memset(s_cfgdev.sbuf, 0, sizeof(s_cfgdev.sbuf));
		if (count <= sizeof(s_cfgdev.sbuf)) {
			errno_t err = memcpy_s(s_cfgdev.sbuf, sizeof(s_cfgdev.sbuf) - 1, buf, count - 1);
			if (err != EOK) {
			    cam_warn("%s mcmcpy fail", __func__);
			}
		} else {
			size_t sbuf_size = sizeof(s_cfgdev.sbuf);
			errno_t err = memcpy_s(s_cfgdev.sbuf, sbuf_size - 1, buf, sbuf_size - 1);
			if (err != EOK) {
				cam_warn("%s mcmcpy fail", __func__);
			}
			s_cfgdev.sbuf[sbuf_size - 1] = '\0';
			cam_warn("%s count[%zu] is beyond sbuf size[%zu]", __func__, count, sizeof(s_cfgdev.sbuf));
		}
		ret = hwcam_cfgdev_guard_thermal();
	}
	cam_debug("%s ret is %d", __func__, ret);
	return count;
}

static ssize_t guard_thermal_show(struct device_driver *drv,
												char *buf)
{
	char *offset = buf;
	int ret;

	cam_info("%s enter", __func__);
	ret = snprintf(offset, PAGE_SIZE, "guard thermal:[%s]\n", s_cfgdev.sbuf);
	offset += ret;
	return (offset - buf);
}

static void
hwcam_cfgdev_dump_meminfo(void)
{
    struct v4l2_event ev =
    {
        .type = HWCAM_V4L2_EVENT_TYPE,
        .id = HWCAM_CFGDEV_REQUEST,
    };
    hwcam_cfgreq2dev_t* req = (hwcam_cfgreq2dev_t*)ev.u.data;

    req->req.intf = NULL;
    req->kind = HWCAM_CFGDEV_REQ_DUMP_MEMINFO;
    hwcam_cfgdev_send_req(NULL, &ev, &s_cfgdev.rq, 1, NULL);
}

static ssize_t dump_meminfo_store(struct device_driver *drv,
												  const char *buf, size_t count)
{
	return strnlen(buf, count);
}

static ssize_t dump_meminfo_show(struct device_driver *drv,
												char *buf)
{
	cam_info("%s enter", __func__);
	hwcam_cfgdev_dump_meminfo();
	return 0;
}

#ifdef DEBUG_HISI_CAMERA
static ssize_t hwcam_log_store(struct device_driver *drv, const char *buf, size_t count)
{
	char *p = (char *)buf;
	u32 val;

	if (p[1] == 'x' || p[1] == 'X' || p[0] == 'x' || p[0] == 'X') {
		p++;
		if (p[0] == 'x' || p[0] == 'X') {
			p++;
		}
		val = simple_strtoul(p, &p, 16);
	} else {
		val = simple_strtoul(p, &p, 10);
	}

	if (p == buf) {
		printk(KERN_INFO ": %s is not in hex or decimal form.\n", buf);
	} else {
		hwcam_debug_mask = val;
	}

	return strnlen(buf, count);
}

static ssize_t hwcam_log_show(struct device_driver *drv, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "0x%08x\n", hwcam_debug_mask);
}
#endif

static void
hwcam_cfgdev_work(
        struct work_struct* w)
{
    mutex_lock(&s_cfgdev_lock);
    mutex_unlock(&s_cfgdev_lock);
}

static void
hwcam_cfgdev_release_ack(
        hwcam_cfgack_t* ack)
{
    kzfree(ack);
}

int
hwcam_cfgdev_queue_ack(
        struct v4l2_event* ev)
{
    hwcam_cfgack_t* ack = NULL;
    hwcam_cfgreq_t* req = (hwcam_cfgreq_t*)ev->u.data;

    if (req->one_way) {
        HWCAM_CFG_ERR("need NOT acknowledge an one way "
                "request(0x%pK, 0x%08x, %u)! \n",
                req->intf, (*(unsigned*)(req + 1)), req->seq);
        return -EINVAL;
    }

    ack = kzalloc(sizeof(hwcam_cfgack_t), GFP_KERNEL);
    if (ack == NULL) {
        HWCAM_CFG_ERR("out of memory for ack! \n");
        return -ENOMEM;
    }
    ack->ev = *ev;
    ack->release = hwcam_cfgdev_release_ack;

    spin_lock(&s_ack_queue_lock);
    list_add_tail(&ack->node, &s_ack_queue);
    wake_up_all(&s_wait_ack);
    spin_unlock(&s_ack_queue_lock);

    return 0;
}

static bool
hwcam_cfgdev_check_ack(
        hwcam_cfgreq_t* req,
        hwcam_cfgack_t** ppack)
{
    bool ret = false;
    hwcam_cfgack_t* ack = NULL;
    hwcam_cfgack_t* tmp = NULL;
    hwcam_cfgreq_t* back = NULL;

    spin_lock(&s_ack_queue_lock);
    ret = s_cfgsvr_flags == HWCAM_CFGSVR_FLAG_UNPLAYED;
    list_for_each_entry_safe(ack, tmp, &s_ack_queue, node) {
        back = (hwcam_cfgreq_t*)ack->ev.u.data;
        if (req->user == back->user
                && req->intf == back->intf
                && req->seq == back->seq) {
            ret = true;
            *ppack = ack;
            list_del(&ack->node);
            break;
        }
    }
    spin_unlock(&s_ack_queue_lock);
    return ret;
}


static void
hwcam_cfgdev_flush_ack_queue(void)
{
    hwcam_cfgack_t* ack = NULL;
    hwcam_cfgack_t* tmp = NULL;

    spin_lock(&s_ack_queue_lock);
    s_cfgsvr_flags = HWCAM_CFGSVR_FLAG_UNPLAYED;
    list_for_each_entry_safe(ack, tmp, &s_ack_queue, node) {
        list_del(&ack->node);
        ack->release(ack);
    }
    wake_up_all(&s_wait_ack);
    spin_unlock(&s_ack_queue_lock);
}

enum
{
    HWCAM_WAIT4ACK_TIME                         =   10000,   // 10s
    HWCAM_WAIT4CFGSVR_TIME                      =   3000,    // 3s
};

static int
hwcam_cfgdev_wait_ack(
        hwcam_user_intf_t* user,
        hwcam_cfgreq_t* req,
        int timeout,
        int* ret)
{
    int rc = -ETIME;
    int retry = 3;
    hwcam_cfgack_t* ack = NULL;
    timeout = msecs_to_jiffies(timeout);

    if (ret) { *ret = -EINVAL; }

    while (true) {
        if (user) {
            hwcam_user_intf_wait_begin(user);
        }
        rc = wait_event_freezable_timeout(s_wait_ack,
                hwcam_cfgdev_check_ack(req, &ack), timeout); /*lint !e666 */
        if (user) {
            hwcam_user_intf_wait_end(user);
        }

        if (ack != NULL) {
            rc = 0;
            break;
        }

        if (0 == rc) {
            HWCAM_CFG_ERR("request(0x%pK, 0x%08x, %u) is out of time for ACK! ",
                    req->intf, (*(unsigned*)(req + 1)), req->seq);
            rc = -ETIME;
            break;
        }
        else if (-ERESTARTSYS == rc) {
            HWCAM_CFG_INFO("request(0x%pK, 0x%08x, %u) is interrupted! pid: %d, tgid: %d, pending.signal: 0x%lx\n",
                    req->intf, (*(unsigned*)(req + 1)), req->seq,
                    current->pid, current->tgid, current->pending.signal.sig[0]);
            rc = -ETIME;
            if (retry > 0) {
                retry--;
                continue;
            } else {
                rc = -ERESTART;
                break;
            }
        }
    }

    if (req->intf) {
        if (ack)  {
            hwcam_cfgreq_intf_on_ack(req->intf, ack);
        }
        else {
            hwcam_cfgreq_intf_on_cancel(req->intf, rc);
        }
    }

    if (ack) {
        if (ret) {
            *ret = hwcam_cfgack_result(ack);
        }
        ack->release(ack);
    }

    return rc;
}

static int
hwcam_cfgdev_thermal_guard(struct v4l2_event* ev)
{
	char* buf = (char *)ev->u.data;
	if (buf != NULL) {
        memcpy(buf, s_cfgdev.sbuf, sizeof(s_cfgdev.sbuf));
    }
	return 0;
}

int
hwcam_cfgdev_send_req(
        hwcam_user_intf_t* user,
        struct v4l2_event* ev,
        struct v4l2_fh* target,
        int one_way,
        int* ret)
{
    int rc = 0;
    hwcam_cfgreq_t* req = (hwcam_cfgreq_t*)ev->u.data;
    req->user = user;
    req->seq = atomic_add_return(1, &s_sequence);
    req->one_way = one_way ? 1 : 0;

    trace_hwcam_cfgdev_send_req_begin(req);

    mutex_lock(&s_cfgdev_lock);
    if (target->vdev) {
        v4l2_event_queue_fh(target, ev);
        if (req->intf) {
            hwcam_cfgreq_intf_get(req->intf);
        }
    }
    else {
        HWCAM_CFG_ERR("the target is invalid!");
        rc = -ENOENT;
    }
    mutex_unlock(&s_cfgdev_lock);

    if (ret) { *ret = rc; }

    if (!rc && !req->one_way) {
        rc = hwcam_cfgdev_wait_ack(
                user, req, HWCAM_WAIT4ACK_TIME, ret);
    }

    trace_hwcam_cfgdev_send_req_end(req);

    return rc;
}

int hw_is_binderized(void)
{
    return is_binderized;
}

char*
gen_media_prefix(char* media_ent,hwcam_device_id_constants_t dev_const, size_t dst_size)
{
    int rc = 0;
    if (dst_size >= 1) {
        rc = snprintf_s(media_ent, dst_size, dst_size-1, "%d",dev_const);
        if (rc < 0) {
            HWCAM_CFG_ERR("snprintf_s media_ent failed");
        }
        strlcat(media_ent, "-" , dst_size);
    }

    return media_ent;
}

int
init_subdev_media_entity(struct v4l2_subdev* subdev,hwcam_device_id_constants_t dev_const)
{
	int rc = 0;
	rc = media_entity_pads_init(&subdev->entity,0,NULL);
	subdev->entity.obj_type = MEDIA_ENTITY_TYPE_V4L2_SUBDEV;
	return rc;
}

static int
hwcam_cfgdev_check_device_status(
        hwcam_dev_intf_t* cam)
{
    int rc = hwcam_cfgpipeline_wait_idle(
            cam, HWCAM_WAIT4CFGSVR_TIME);
    if (!rc) {
        mutex_lock(&s_cfgsvr_lock);
        if (s_pid_cfgsvr) {
            HWCAM_CFG_ERR(
                    "the server stalled, "
                    "try restarting it! ");
            kill_pid(s_pid_cfgsvr, SIGKILL, 1);
        }
        mutex_unlock(&s_cfgsvr_lock);
    }
    return rc;
}

static bool
hwcam_cfgdev_is_cfgsvr_running(void)
{
    bool rc = false;
    if (s_pid_cfgsvr) {
        rcu_read_lock();
        rc = pid_task(s_pid_cfgsvr, PIDTYPE_PID) != NULL;
        rcu_read_unlock();
    }
    return rc;
}

static int
hwcam_cfgdev_check_cfgsvr_status(void)
{
    int rc = 0;
    mutex_lock(&s_cfgsvr_lock);
    rc = wait_event_timeout(s_wait_cfgsvr,
            hwcam_cfgdev_is_cfgsvr_running(),
            msecs_to_jiffies(HWCAM_WAIT4CFGSVR_TIME)); //lint !e666
    if (!rc) {
        HWCAM_CFG_ERR("the server is not running!");
    }
    mutex_unlock(&s_cfgsvr_lock);
    return rc;
}

int
hwcam_cfgdev_mount_pipeline(
        hwcam_user_intf_t* user,
        hwcam_dev_intf_t* cam,
        int moduleID,
        hwcam_cfgpipeline_intf_t** pl)
{
    int rc = -ENOMEM;
    int ack = 0;
    struct v4l2_event ev =
    {
        .type = HWCAM_V4L2_EVENT_TYPE,
        .id = HWCAM_CFGDEV_REQUEST,
    };
    hwcam_cfgreq2dev_t* req = (hwcam_cfgreq2dev_t*)ev.u.data;
    hwcam_cfgreq_mount_pipeline_intf_t* mpr = NULL;
    *pl = NULL;

    if (!hwcam_cfgdev_check_device_status(cam)) {
        rc = -EBUSY;
        goto exit_mount_pipeline;
    }
    if (!hwcam_cfgdev_check_cfgsvr_status()) {
        rc = -ENOENT;
        goto exit_mount_pipeline;
    }

    rc = hwcam_cfgpipeline_mount_req_create_instance(
            s_cfgdev.vdev, cam, moduleID, &mpr);
    if (!mpr) {
        goto exit_mount_pipeline;
    }

    req->req.intf = (hwcam_cfgreq_intf_t*)mpr;
    req->kind = HWCAM_CFGDEV_REQ_MOUNT_PIPELINE;

    rc = hwcam_cfgdev_send_req(user, &ev, &s_cfgdev.rq, 0, &ack);
    if (rc) {
        goto fail_mount_pipeline;
    }
    rc = ack;

    hwcam_cfgreq_mount_pipeline_intf_get_result(mpr, pl);
    if (!*pl) {
        goto fail_mount_pipeline;
    }

fail_mount_pipeline:
    hwcam_cfgreq_intf_put((hwcam_cfgreq_intf_t*)mpr);
    if(-ERESTART == rc){
        mutex_lock(&s_cfgsvr_lock);
        if (s_pid_cfgsvr) {
            HWCAM_CFG_ERR(
                  "the mount operation was interrupted, "
                  "try restarting it! ");
            kill_pid(s_pid_cfgsvr, SIGKILL, 1);
        }
        mutex_unlock(&s_cfgsvr_lock);
    }
exit_mount_pipeline:
    return rc;
}

static void
hwcam_cfgdev_subdev_release(
        struct video_device *vdev)
{
    struct v4l2_subdev *sd = video_get_drvdata(vdev);
    if(sd == NULL)
        return;
    sd->devnode = NULL;
    video_device_release(vdev);

    HWCAM_CFG_INFO("TODO. \n");
}

static unsigned int
hwcam_subdev_poll(
        struct file* file,
        poll_table* wait)
{
	struct video_device* vdev = video_devdata(file);
	struct v4l2_subdev* sd = vdev_to_v4l2_subdev(vdev);
	struct v4l2_fh* fh = file->private_data;

	if(sd==NULL){
        return POLLERR;
	}

	if (!(sd->flags & V4L2_SUBDEV_FL_HAS_EVENTS)) {
        return POLLERR;
    }

	poll_wait(file, &fh->wait, wait);

	if (v4l2_event_pending(fh)) {
		return POLLIN;
    }

	return 0;
}

static struct v4l2_file_operations
hwcam_v4l2_subdev_fops =
{
};

int
hwcam_cfgdev_register_subdev(
        struct v4l2_subdev* sd,hwcam_device_id_constants_t dev_const)
{
    int rc = 0;
    int ret = 0;
    int name_len = 0;
    char media_prefix[10];
    struct video_device* vdev = NULL;
    struct v4l2_device* v4l2 = &s_cfgdev.v4l2;

    if (!sd || !sd->name[0]) {
	rc = -EINVAL;
        goto out;
    }

    rc = v4l2_device_register_subdev(v4l2, sd);
    if (rc < 0) {
        goto out;
    }

    if (!(sd->flags & V4L2_SUBDEV_FL_HAS_DEVNODE)) {
        goto out;
    }

    vdev = video_device_alloc();
    if (!vdev) {
        rc = -ENOMEM;
        goto video_alloc_fail;
    }

    video_set_drvdata(vdev, sd);
    gen_media_prefix(media_prefix, dev_const, sizeof(media_prefix));
    ret = snprintf_s(vdev->name, sizeof(vdev->name), sizeof(vdev->name) - 1, "%s", media_prefix);
    if (ret < 0) {
        HWCAM_CFG_ERR("snprintf_s media_prefix failed");
    }
    strlcpy(vdev->name + strlen(vdev->name), sd->name, sizeof(vdev->name) - strlen(vdev->name));
    name_len = strlen(vdev->name);
    vdev->v4l2_dev = v4l2;
    vdev->fops = &hwcam_v4l2_subdev_fops;
    vdev->release = hwcam_cfgdev_subdev_release;
    rc = __video_register_device(vdev, VFL_TYPE_SUBDEV, -1, 1, sd->owner);
    if (rc < 0) {
        goto video_register_fail;
    }
	cam_debug("register video devices %s sucessful", sd->name);
	cam_debug("video dev name %s %s", vdev->dev.kobj.name, vdev->name);
    sd->entity.info.dev.major = VIDEO_MAJOR;
    sd->entity.info.dev.minor = vdev->minor;
    rc = snprintf(vdev->name + strlen(vdev->name),sizeof(vdev->name) - strlen(vdev->name),"%s",video_device_node_name(vdev));
    if(rc > AVAIL_NAME_LENGTH - name_len){
	HWCAM_CFG_ERR("Truncation Occurred\n");
	snprintf(vdev->name,sizeof(vdev->name),"%s",media_prefix);
	snprintf(vdev->name + strlen(vdev->name),sizeof(vdev->name) - strlen(vdev->name),"%s",video_device_node_name(vdev));
	rc = 0;
   }
   rc = 0;
   sd->entity.name = vdev->name;
   sd->devnode = vdev;
   goto out;

video_register_fail:
    video_device_release(vdev);

video_alloc_fail:
    if (sd->devnode) {
        v4l2_device_unregister_subdev(sd);
    }

out:
    return rc;
}

int
hwcam_cfgdev_unregister_subdev(
        struct v4l2_subdev* sd)
{
	if (!sd) {
		return -EINVAL;
    }
	v4l2_device_unregister_subdev(sd);
	return 0;
}

static unsigned int
hwcam_cfgdev_vo_poll(
        struct file* filep,
        struct poll_table_struct* ptbl)
{
	unsigned int rc = 0;
	mutex_lock(&s_cfgdev.lock);
	poll_wait(filep, &s_cfgdev.rq.wait, ptbl);
	if (v4l2_event_pending(&s_cfgdev.rq)) {
		rc = POLLIN | POLLRDNORM;
    }
	mutex_unlock(&s_cfgdev.lock);
	return rc;
}

static long
hwcam_cfgdev_vo_dqevent(
        struct v4l2_event* ev,
        int nb)
{
    long rc = 0;
    hwcam_cfgreq_t* req = NULL;
    while (true) {
        rc = v4l2_event_dequeue(&s_cfgdev.rq, ev, nb);
        if (rc != 0) { break; }

        req = (hwcam_cfgreq_t*)ev->u.data;
        if (req->intf) {
            rc = hwcam_cfgreq_intf_on_req(req->intf, ev);
            hwcam_cfgreq_intf_put(req->intf);
            if (rc) {
                HWCAM_CFG_ERR("request(0x%pK, 0x%08x, %u) skipped. \n",
                        req->intf, (*(unsigned*)(req + 1)), req->seq);
                continue;   // to skip failed/timeouted/interrupted events
            }
        }
        break;
    }
    return rc;
}

static void
hwcam_cfgdev_subscribed_event_ops_merge(
        const struct v4l2_event* old,
        struct v4l2_event* new)
{
    hwcam_cfgreq2dev_t* req = (hwcam_cfgreq2dev_t*)&old->u.data;
    if (req->req.intf) {
        hwcam_cfgreq_intf_put(req->req.intf);
    }
    HWCAM_CFG_ERR("the event queue overflowed! \n");
}

static struct v4l2_subscribed_event_ops
s_hwcam_subscribed_event_ops =
{
    .merge = hwcam_cfgdev_subscribed_event_ops_merge,
};

static long
hwcam_cfgdev_vo_do_ioctl(
        struct file* filep,
        unsigned int cmd,
        void* arg)
{
    long rc = -EINVAL;
    switch (cmd)
    {
    case VIDIOC_DQEVENT:
        rc = hwcam_cfgdev_vo_dqevent(arg,
                filep->f_flags & O_NONBLOCK);
        break;

    case VIDIOC_SUBSCRIBE_EVENT:
        rc = v4l2_event_subscribe(&s_cfgdev.rq,
                (struct v4l2_event_subscription*)arg, 8,
                &s_hwcam_subscribed_event_ops);
        break;

    case VIDIOC_UNSUBSCRIBE_EVENT:
        rc = v4l2_event_unsubscribe(&s_cfgdev.rq,
                (struct v4l2_event_subscription*)arg);
        break;

    case HWCAM_V4L2_IOCTL_REQUEST_ACK:
        rc = hwcam_cfgdev_queue_ack((struct v4l2_event*)arg);
        break;

    case HWCAM_V4L2_IOCTL_THERMAL_GUARD:
        rc = hwcam_cfgdev_thermal_guard((struct v4l2_event*)arg);
        break;

    default:
        HWCAM_CFG_ERR("invalid IOCTL CMD(%d)! \n", cmd);
        break;
    }
    return rc;
}

static long
hwcam_cfgdev_vo_ioctl(
        struct file* filep,
        unsigned int cmd,
        unsigned long arg)
{
    return video_usercopy(filep,
            cmd, arg, hwcam_cfgdev_vo_do_ioctl);
}

#ifdef CONFIG_COMPAT
static long
hwcam_cfgdev_vo_ioctl32(
        struct file* filep,
        unsigned int cmd,
        unsigned long arg)
{
    long rc = 0;
    void __user *up_p = NULL;
    void __user *kp = NULL;
    up_p = compat_ptr(arg);

	switch (cmd)
	{
	case HWCAM_V4L2_IOCTL_REQUEST_ACK32: cmd = HWCAM_V4L2_IOCTL_REQUEST_ACK; break;
	case HWCAM_V4L2_IOCTL_THERMAL_GUARD32: cmd = HWCAM_V4L2_IOCTL_THERMAL_GUARD; break;
	}

	switch (cmd)
    {
    case HWCAM_V4L2_IOCTL_REQUEST_ACK:
	case HWCAM_V4L2_IOCTL_THERMAL_GUARD:
		{
			kp = compat_alloc_user_space(sizeof(struct v4l2_event));
			if (NULL == kp)
				return -EFAULT;
			rc = compat_get_v4l2_event_data(kp, up_p);
			if (0 != rc)
				return rc;
			rc = hwcam_cfgdev_vo_ioctl(filep, cmd, (unsigned long)(uintptr_t)(kp));
			if (0 != rc)
				return rc;
			rc = compat_put_v4l2_event_data(kp, up_p);
		}
		break;
    default:
        rc = hwcam_cfgdev_vo_ioctl(filep, cmd, arg);
        break;
    }
    return rc;
}
#endif

static int
hwcam_cfgdev_vo_close(
        struct file* filep)
{
    void* fpd = NULL;
    mutex_lock(&s_cfgdev.lock);
    swap(filep->private_data, fpd);
    mutex_unlock(&s_cfgdev.lock);
    if (fpd) {
        struct pid* pid = NULL;

        mutex_lock(&s_cfgsvr_lock);
        swap(s_pid_cfgsvr, pid);

        mutex_lock(&s_cfgdev_lock);
        v4l2_fh_del(&s_cfgdev.rq);
        v4l2_fh_exit(&s_cfgdev.rq);
        mutex_unlock(&s_cfgdev_lock);

        if (pid) {
            put_pid(pid);
        }

        mutex_unlock(&s_cfgsvr_lock);
        hwcam_cfgdev_flush_ack_queue();

        HWCAM_CFG_INFO("the server(%d) detached. \n", current->pid);
    }
    return 0;
}

static int
hwcam_cfgdev_vo_open(
        struct file* filep)
{
    int rc = 0;

    mutex_lock(&s_cfgsvr_lock);
    if (s_pid_cfgsvr) {
        mutex_unlock(&s_cfgsvr_lock);
        HWCAM_CFG_INFO("only one server can attach to cfgdev! \n");
        rc = -EBUSY;
        goto exit_open;
    }
    s_pid_cfgsvr = get_pid(task_pid(current));

    mutex_lock(&s_cfgdev_lock);
	v4l2_fh_init(&s_cfgdev.rq, s_cfgdev.vdev);
    v4l2_fh_add(&s_cfgdev.rq);
    mutex_unlock(&s_cfgdev_lock);
    mutex_unlock(&s_cfgsvr_lock);

    spin_lock(&s_ack_queue_lock);
    s_cfgsvr_flags = HWCAM_CFGSVR_FLAG_PLAYING;
    spin_unlock(&s_ack_queue_lock);

    filep->private_data = &s_cfgdev;
    wake_up_all(&s_wait_cfgsvr);

    HWCAM_CFG_INFO("the server(%d) attached. \n", current->pid);

exit_open:
	return rc;
}

static int hwcam_cfgdev_get_dts(struct platform_device* pDev)
{
    struct device *pdev = NULL;
    struct device_node *np = NULL;
    int rc = 0;

    if (NULL == pDev) {
        HWCAM_CFG_ERR("pDev NULL.");
        return -ENOMEM;
    }

    pdev = &(pDev->dev);

    if (NULL == pdev) {
        HWCAM_CFG_ERR("pdev NULL.");
        return -ENOMEM;
    }

    np = pdev->of_node;

    if (NULL == np) {
        HWCAM_CFG_ERR("of node NULL.");
        return -ENOMEM;
    }

    rc = of_property_read_u32(np, "huawei,binderized", (u32*)(&is_binderized));
    if (rc < 0) {
        HWCAM_CFG_ERR("get binderized flag failed.");
        return -ENOMEM;
    }

    HWCAM_CFG_INFO("binderized=%d", is_binderized);

    return 0;
}

static struct v4l2_file_operations
s_fops_cfgdev =
{
	.owner = THIS_MODULE,
	.open = hwcam_cfgdev_vo_open,
	.poll = hwcam_cfgdev_vo_poll,
	.unlocked_ioctl = hwcam_cfgdev_vo_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl32 = hwcam_cfgdev_vo_ioctl32,
#endif
	.release = hwcam_cfgdev_vo_close,
};

static void
hwcam_cfgdev_vo_subdev_notify(
        struct v4l2_subdev* sd,
        unsigned int notification,
        void* arg)
{
    HWCAM_CFG_INFO("TODO. \n");
}

static int
hwcam_cfgdev_vo_probe(
        struct platform_device* pdev)
{
    int rc = 0;
    int ret = 0;
    int name_len = 0;
    char media_prefix[10];
    struct video_device* vdev = NULL;
    struct media_device* mdev = NULL;
    struct v4l2_device* v4l2 = &s_cfgdev.v4l2;

    vdev = video_device_alloc();
    if (!vdev) {
        rc = -ENOMEM;
        goto probe_end;
    }

    mdev = kzalloc(sizeof(struct media_device), GFP_KERNEL);
    if (!mdev) {
        rc = -ENOMEM;
        goto media_alloc_fail;
    }

    rc = hwcam_cfgdev_get_dts(pdev);
    if (rc < 0) {
        HWCAM_CFG_DEBUG("get dts failed.");
    }

    media_device_init(mdev);
    strlcpy(mdev->model, HWCAM_MODEL_CFG, sizeof(mdev->model));
    mdev->dev = &(pdev->dev);
    rc = media_device_register(mdev);
    if (rc < 0) {
        goto media_register_fail;
    }

    rc = media_entity_pads_init(&vdev->entity,0,NULL);
    if (rc < 0) {
 	goto entity_init_fail;
    }

    v4l2->mdev = mdev;
    v4l2->notify = hwcam_cfgdev_vo_subdev_notify;
    rc = v4l2_device_register(&(pdev->dev), v4l2);
    if (rc < 0) {
	goto v4l2_register_fail;
    }

    vdev->v4l2_dev = v4l2;
    gen_media_prefix(media_prefix,HWCAM_VNODE_GROUP_ID, sizeof(media_prefix));
    ret = snprintf_s(vdev->name, sizeof(vdev->name), sizeof(vdev->name) - 1, "%s", media_prefix);
    if (ret < 0) {
        HWCAM_CFG_ERR("snprintf_s media_prefix failed");
    }
    strlcpy(vdev->name + strlen(vdev->name), "hwcam-cfgdev", sizeof(vdev->name) - strlen(vdev->name));
    name_len = strlen(vdev->name);
    vdev->entity.obj_type = MEDIA_ENTITY_TYPE_VIDEO_DEVICE;
    vdev->release = video_device_release_empty;
    vdev->fops = &s_fops_cfgdev;
    vdev->minor = -1;
    vdev->vfl_type = VFL_TYPE_GRABBER;
    rc = video_register_device(vdev, VFL_TYPE_GRABBER, -1);
    if (rc < 0) {
        goto video_register_fail;
    }
    cam_debug("video dev name %s %s",vdev->dev.kobj.name,vdev->name);
    rc = snprintf(vdev->name + strlen(vdev->name),sizeof(vdev->name) - strlen(vdev->name),"%s",video_device_node_name(vdev));
    if(rc > AVAIL_NAME_LENGTH - name_len){
        HWCAM_CFG_ERR("Truncation Occurred\n");
        snprintf(vdev->name, sizeof(vdev->name),"%s",media_prefix);
        snprintf(vdev->name + strlen(vdev->name),sizeof(vdev->name) - strlen(vdev->name),"%s",video_device_node_name(vdev));
        rc = 0;
    }
    rc = 0;
    vdev->entity.name = vdev->name;
    vdev->lock = &s_cfgdev_lock;
    video_set_drvdata(vdev, &s_cfgdev);

    s_cfgdev.vdev = vdev;
    s_cfgdev.mdev = mdev;

    s_cfgdev.debug_root = debugfs_create_dir("hwcam", NULL);
    mutex_init(&s_cfgdev.lock);

    if(client_camera_user == NULL)
    {
        client_camera_user= dsm_register_client(&dev_camera_user);
    }

    goto probe_end;

video_register_fail:
    v4l2_device_unregister(v4l2);

v4l2_register_fail:
    media_entity_cleanup(&vdev->entity);

entity_init_fail:
    media_device_unregister(mdev);

media_register_fail:
    kzfree(mdev);

media_alloc_fail:
    video_device_release(vdev);

probe_end:
    HWCAM_CFG_INFO("exit");
    return rc;
}

static int
hwcam_cfgdev_vo_remove(
    struct platform_device* pdev)
{
    video_unregister_device(s_cfgdev.vdev);
    v4l2_device_unregister(&s_cfgdev.v4l2);
    media_entity_cleanup(&(s_cfgdev.vdev)->entity);
    media_device_unregister(s_cfgdev.mdev);
    kzfree(s_cfgdev.mdev);
    video_device_release(s_cfgdev.vdev);
    s_cfgdev.vdev = NULL;
    s_cfgdev.mdev = NULL;
    mutex_destroy(&s_cfgdev.lock);
    return 0;
}

static const struct of_device_id
s_cfgdev_devtbl_match[] =
{
    {
        .compatible = "huawei,camcfgdev"
    },
    {
    },
}

MODULE_DEVICE_TABLE(of, s_cfgdev_devtbl_match);

static struct platform_driver
s_cfgdev_driver =
{
    .probe = hwcam_cfgdev_vo_probe,
    .remove = hwcam_cfgdev_vo_remove,
    .driver =
    {
        .name = "huawei,camcfgdev",
        .owner = THIS_MODULE,
        .of_match_table = s_cfgdev_devtbl_match,
    },
};

static int __init
hwcam_cfgdev_vo_init(void)
{
    int ret = 0;
    hwcam_v4l2_subdev_fops = v4l2_subdev_fops;
    hwcam_v4l2_subdev_fops.poll = hwcam_subdev_poll;
    ret = platform_driver_register(&s_cfgdev_driver);
	if(ret == 0) {
		if (driver_create_file(&s_cfgdev_driver.driver, &driver_attr_guard_thermal)) {
			cam_warn("%s create driver attr failed", __func__);
		}

		if (driver_create_file(&s_cfgdev_driver.driver, &driver_attr_dump_meminfo)) {
			cam_warn("%s create driver attr dump_meminfo failed", __func__);
		}

#ifdef DEBUG_HISI_CAMERA
		if (driver_create_file(&s_cfgdev_driver.driver, &driver_attr_hwcam_log)) {
			cam_warn("%s create driver attr hwcam_log failed", __func__);
		}
#endif
	}
    return ret;
}

static void __exit
hwcam_cfgdev_vo_exit(void)
{
    platform_driver_unregister(&s_cfgdev_driver);
}

module_init(hwcam_cfgdev_vo_init);
module_exit(hwcam_cfgdev_vo_exit);
MODULE_DESCRIPTION("Huawei V4L2 Camera");
MODULE_LICENSE("GPL v2");
//lint -restore

