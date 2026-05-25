#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/select.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <dirent.h>
#include <math.h>

#define DEFAULT_MAX_SLOTS 10
#define DEFAULT_DEVICE_NAME "fts"  // 默认设备名称

// 用于检查设备能力的宏
#define NBITS(x) ((((x)-1)/(sizeof(long)*8))+1)
#define test_bit(bit, array) ((array[bit/(sizeof(long)*8)] >> (bit%(sizeof(long)*8))) & 1)

// 真实设备的能力信息
typedef struct {
    char name[256];
    int max_x;
    int max_y;
    int max_pressure;
    int max_touch_major;
    int max_orientation;
    int has_pressure;
    int has_touch_major;
    int has_touch_minor;
    int has_orientation;
} real_device_info;

struct touch_ids {
    int ids[DEFAULT_MAX_SLOTS];
    int next_id;
};

// 查找真实触摸屏设备并分析能力
static int find_real_touchscreen_info(real_device_info *info) {
    DIR *dir;
    struct dirent *entry;
    char path[256];
    int fd;
    char device_name[256];
    char best_device_name[256] = {0};
    char best_device_path[256] = {0};
    int best_fd = -1;
    int priority = 0;  // 优先级：touchscreen=3, touch=2, 其他=1
    
    dir = opendir("/dev/input");
    if (!dir) {
        return -1;
    }
    
    // 遍历 /dev/input/eventX 设备，找到优先级最高的触摸屏
    while ((entry = readdir(dir)) != NULL) {
        if (strncmp(entry->d_name, "event", 5) != 0) {
            continue;
        }
        
        snprintf(path, sizeof(path), "/dev/input/%s", entry->d_name);
        fd = open(path, O_RDONLY);
        if (fd < 0) {
            continue;
        }
        
        // 获取设备名称
        if (ioctl(fd, EVIOCGNAME(sizeof(device_name)), device_name) < 0) {
            close(fd);
            continue;
        }
        
        // 检查是否是触摸屏设备（检查关键能力）
        unsigned long bit[EV_MAX][NBITS(KEY_MAX)];
        memset(bit, 0, sizeof(bit));
        
        if (ioctl(fd, EVIOCGBIT(0, EV_MAX), bit[0]) < 0) {
            close(fd);
            continue;
        }
        
        // 检查是否支持 ABS 事件和 KEY 事件
        if (!test_bit(EV_ABS, bit[0]) || !test_bit(EV_KEY, bit[0])) {
            close(fd);
            continue;
        }
        
        // 检查是否支持多点触摸
        if (ioctl(fd, EVIOCGBIT(EV_ABS, KEY_MAX), bit[EV_ABS]) < 0) {
            close(fd);
            continue;
        }
        
        if (test_bit(ABS_MT_POSITION_X, bit[EV_ABS]) && 
            test_bit(ABS_MT_POSITION_Y, bit[EV_ABS])) {
            
            // 计算优先级：touchscreen(主屏) > touchscreen2(副屏) > touch > 其他
            // 折叠屏手机（如三星Z Flip）有多个触摸屏：
            // - sec_touchscreen (主屏) -> 优先级4
            // - sec_touchscreen2 (外屏/副屏) -> 优先级3
            int current_priority = 1;
            if (strstr(device_name, "touchscreen") != NULL) {
                // 检查是否是主触摸屏（名称以touchscreen结尾，不带数字后缀）
                // 例如：sec_touchscreen 是主屏，sec_touchscreen2 是副屏
                int name_len = strlen(device_name);
                int is_primary = 1;
                // 检查最后一个字符是否是数字（副屏通常带数字后缀）
                if (name_len > 0 && device_name[name_len - 1] >= '0' && device_name[name_len - 1] <= '9') {
                    is_primary = 0;  // 带数字后缀，是副屏
                }
                current_priority = is_primary ? 4 : 3;  // 主屏优先级更高
            } else if (strstr(device_name, "touch") != NULL && 
                       strstr(device_name, "touchpad") == NULL) {
                current_priority = 2;
            }
            
            // 如果找到更高优先级的设备，替换
            if (current_priority > priority) {
                if (best_fd >= 0) {
                    close(best_fd);
                }
                priority = current_priority;
                best_fd = fd;
                strncpy(best_device_name, device_name, sizeof(best_device_name) - 1);
                strncpy(best_device_path, path, sizeof(best_device_path) - 1);
                continue;  // 不要关闭 fd，继续查找
            }
            
            close(fd);
        } else {
            close(fd);
        }
    }
    
    closedir(dir);
    
    if (best_fd < 0) {
        return -1;
    }
    
    // 分析找到的最佳设备
    fd = best_fd;
    strncpy(device_name, best_device_name, sizeof(device_name));
    strncpy(path, best_device_path, sizeof(path));
    
    unsigned long bit[EV_MAX][NBITS(KEY_MAX)];
    memset(bit, 0, sizeof(bit));
    ioctl(fd, EVIOCGBIT(0, EV_MAX), bit[0]);
    ioctl(fd, EVIOCGBIT(EV_ABS, KEY_MAX), bit[EV_ABS]);
    ioctl(fd, EVIOCGBIT(EV_KEY, KEY_MAX), bit[EV_KEY]);
    
    {
            // 找到真实触摸屏设备，静默获取信息
    }
    
    // 收集设备信息
    memset(info, 0, sizeof(real_device_info));
    strncpy(info->name, device_name, sizeof(info->name) - 1);
    
    // 检测能力
    info->has_pressure = test_bit(ABS_MT_PRESSURE, bit[EV_ABS]);
    info->has_touch_major = test_bit(ABS_MT_TOUCH_MAJOR, bit[EV_ABS]);
    info->has_touch_minor = test_bit(ABS_MT_TOUCH_MINOR, bit[EV_ABS]);
    info->has_orientation = test_bit(ABS_MT_ORIENTATION, bit[EV_ABS]);
    
    // 获取参数范围
    struct input_absinfo absinfo;
    if (ioctl(fd, EVIOCGABS(ABS_MT_POSITION_X), &absinfo) == 0) {
        info->max_x = absinfo.maximum;
    }
    if (ioctl(fd, EVIOCGABS(ABS_MT_POSITION_Y), &absinfo) == 0) {
        info->max_y = absinfo.maximum;
    }
    if (info->has_pressure && ioctl(fd, EVIOCGABS(ABS_MT_PRESSURE), &absinfo) == 0) {
        info->max_pressure = absinfo.maximum;
    }
    if (info->has_touch_major && ioctl(fd, EVIOCGABS(ABS_MT_TOUCH_MAJOR), &absinfo) == 0) {
        info->max_touch_major = absinfo.maximum;
    }
    if (info->has_orientation && ioctl(fd, EVIOCGABS(ABS_MT_ORIENTATION), &absinfo) == 0) {
        info->max_orientation = absinfo.maximum;
    }
    
    close(fd);
    return 0;
}

// 触摸面积随机生成（模拟 VirtualTouch.cc 的加权随机逻辑）
#define TOUCH_MAX_VALUE 255    // 统一使用 255 作为最大值

// 随机数生成器（LCG算法）
static unsigned int touch_seed = 0;

static void init_touch_random(void) {
    if (touch_seed == 0) {
        struct timeval tv;
        gettimeofday(&tv, NULL);
        touch_seed = (unsigned int)(tv.tv_sec ^ tv.tv_usec);
    }
}

static double get_random_double(void) {
    touch_seed = touch_seed * 1103515245 + 12345;
    return ((touch_seed / 65536) % 10000) / 10000.0;  // 0.0~1.0
}

// 加权随机生成触摸面积（优化版，避免0值）
// 50% → 0.015, 35% → 0.01, 15% → 0.02
// 真实手指点击不可能是0面积
static int get_random_touch_major(void) {
    double weight = get_random_double();
    double size_val;
    if (weight < 0.50) {
        size_val = 0.015;     // 50% → 正常触摸 (0.015*255≈4)
    } else if (weight < 0.85) {
        size_val = 0.01;      // 35% → 轻触 (0.01*255≈3)
    } else {
        size_val = 0.02;      // 15% → 重按 (0.02*255≈5)
    }
    return (int)(size_val * TOUCH_MAX_VALUE);
}

// 随机生成触摸方向（20~70）
static int get_random_orientation(void) {
    double orientation_float = 0.4 + get_random_double();  // 0.4~1.4
    return (int)(orientation_float * 50);  // 20~70
}

// 生成坐标随机偏移（±0.5像素）
// 模拟真实手指不可能每次点击完全相同的位置
static double get_coord_offset(void) {
    double random_val = get_random_double();  // 0.0~1.0
    return (random_val - 0.5);  // -0.5~0.5
}

// 坐标转换：根据屏幕方向转换坐标
// orientation: 0=竖屏（不转换），1=横屏（转换为竖屏物理坐标）
// 设备的物理坐标系始终是竖屏的
static void transform_coords(int *x, int *y, int orientation, int width, int height) {
    if (orientation == 1) {
        int temp_x = width - *y;
        int temp_y = *x;
        *x = temp_x;
        *y = temp_y;
    } else if (orientation == 3) {
        int temp_x = *y;
        int temp_y = height- *x;
        *x = temp_x;
        *y = temp_y;
    }
    // orientation == 0 时不转换（竖屏）
}

static int send_event(int fd, unsigned short type, unsigned short code, int value) {
    struct input_event ev = {0};
    struct timeval tv;
    gettimeofday(&tv, NULL);
    ev.time = tv;
    ev.type = type;
    ev.code = code;
    ev.value = value;
    ssize_t w = write(fd, &ev, sizeof(ev));
    return w == sizeof(ev) ? 0 : -1;
}

static int sync_report(int fd) {
    // MT Protocol B 只需要 SYN_REPORT，不需要 SYN_MT_REPORT
    // 发送 SYN_MT_REPORT 会导致滑动失败！
    return send_event(fd, EV_SYN, SYN_REPORT, 0);
}

static int setup_abs(int fd, unsigned int code, int min, int max, int fuzz, int flat) {
    struct uinput_abs_setup abs = {0};
    abs.code = code;
    abs.absinfo.minimum = min;
    abs.absinfo.maximum = max;
    abs.absinfo.resolution = 1;
    abs.absinfo.fuzz = fuzz;
    abs.absinfo.flat = flat;
    return ioctl(fd, UI_ABS_SETUP, &abs);
}

static int setup_device(const real_device_info *real_info, int max_slots) {
    int fd = -1;

    // 自动尝试 uinput 设备路径（与 VirtualTouch.cc 逻辑一致）
    // 1. 先尝试现代 Android 路径
    fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (fd < 0) {
        // 2. 尝试旧版 Android 路径
        fd = open("/dev/misc/uinput", O_WRONLY | O_NONBLOCK);
    }

    if (fd < 0) {
        perror("open uinput (tried /dev/uinput and /dev/misc/uinput)");
        return -1;
    }

    if (ioctl(fd, UI_SET_EVBIT, EV_SYN) < 0 ||
        ioctl(fd, UI_SET_EVBIT, EV_ABS) < 0 ||
        ioctl(fd, UI_SET_EVBIT, EV_KEY) < 0 ||
        ioctl(fd, UI_SET_KEYBIT, BTN_TOUCH) < 0 ||
        ioctl(fd, UI_SET_KEYBIT, BTN_TOOL_FINGER) < 0 ||  // 所有真实设备都支持（实测）
        ioctl(fd, UI_SET_PROPBIT, INPUT_PROP_DIRECT) < 0) { // 标记为触摸屏，避免被识别成触控板指针
        perror("ioctl set event bits");
        close(fd);
        return -1;
    }

    // Multi-touch Protocol B (Android 7+ 标准协议)
    // 基础能力（必须）
    if (ioctl(fd, UI_SET_ABSBIT, ABS_MT_SLOT) < 0 ||
        ioctl(fd, UI_SET_ABSBIT, ABS_MT_TRACKING_ID) < 0 ||
        ioctl(fd, UI_SET_ABSBIT, ABS_MT_POSITION_X) < 0 ||
        ioctl(fd, UI_SET_ABSBIT, ABS_MT_POSITION_Y) < 0) {
        perror("ioctl set multi-touch bits");
        close(fd);
        return -1;
    }

    // 根据真实设备的能力，动态设置虚拟设备的能力
    if (real_info->has_touch_major) {
        if (ioctl(fd, UI_SET_ABSBIT, ABS_MT_TOUCH_MAJOR) < 0) {
            perror("ioctl set TOUCH_MAJOR");
            close(fd);
            return -1;
        }
    }

    if (real_info->has_touch_minor) {
        if (ioctl(fd, UI_SET_ABSBIT, ABS_MT_TOUCH_MINOR) < 0) {
            perror("ioctl set TOUCH_MINOR");
            close(fd);
            return -1;
        }
    }

    // 注意：不声明 PRESSURE，因为所有测试设备（三星/红米/小米）都不支持

    if (real_info->has_orientation) {
        if (ioctl(fd, UI_SET_ABSBIT, ABS_MT_ORIENTATION) < 0) {
            perror("ioctl set ORIENTATION");
            close(fd);
            return -1;
        }
    }

    // 设置 Multi-touch Protocol B 参数范围（使用真实设备的范围）
    // fuzz=0, flat=0 与真实设备一致
    if (setup_abs(fd, ABS_MT_SLOT, 0, max_slots - 1, 0, 0) < 0 ||
        setup_abs(fd, ABS_MT_TRACKING_ID, 0, 0xFFFF, 0, 0) < 0 ||
        setup_abs(fd, ABS_MT_POSITION_X, 0, real_info->max_x, 0, 0) < 0 ||
        setup_abs(fd, ABS_MT_POSITION_Y, 0, real_info->max_y, 0, 0) < 0) {
        perror("ioctl abs setup (basic)");
        close(fd);
        return -1;
    }

    // 根据真实设备动态设置参数范围（使用固定最大值 255）
    if (real_info->has_touch_major) {
        if (setup_abs(fd, ABS_MT_TOUCH_MAJOR, 0, TOUCH_MAX_VALUE, 0, 0) < 0) {
            perror("ioctl abs setup TOUCH_MAJOR");
            close(fd);
            return -1;
        }
    }

    if (real_info->has_touch_minor) {
        if (setup_abs(fd, ABS_MT_TOUCH_MINOR, 0, TOUCH_MAX_VALUE, 0, 0) < 0) {
            perror("ioctl abs setup TOUCH_MINOR");
            close(fd);
            return -1;
        }
    }

    // 注意：不设置 PRESSURE 范围，因为所有测试设备都不支持

    if (real_info->has_orientation) {
        if (setup_abs(fd, ABS_MT_ORIENTATION, -90, 90, 0, 0) < 0) {
            perror("ioctl abs setup ORIENTATION");
            close(fd);
            return -1;
        }
    }

    struct uinput_setup usetup = {0};
    usetup.id.bustype = BUS_VIRTUAL;
    usetup.id.vendor = 0x18D1;   // Google
    usetup.id.product = 0x4E26;
    usetup.id.version = 1;

    // 使用真实设备的名称
    snprintf(usetup.name, UINPUT_MAX_NAME_SIZE, "%s", real_info->name);

    if (ioctl(fd, UI_DEV_SETUP, &usetup) < 0) {
        perror("UI_DEV_SETUP");
        close(fd);
        return -1;
    }
    if (ioctl(fd, UI_DEV_CREATE) < 0) {
        perror("UI_DEV_CREATE");
        close(fd);
        return -1;
    }

    // Give kernel time to create the device node.
    usleep(100 * 1000);
    return fd;
}

static int ensure_slot(int fd, struct touch_ids *ids, int slot) {
    if (slot < 0 || slot >= DEFAULT_MAX_SLOTS) return -1;
    if (ids->ids[slot] < 0) {
        ids->ids[slot] = ids->next_id++;
    }
    // 真实设备不发送 ABS_MT_SLOT，只发送 TRACKING_ID
    // if (send_event(fd, EV_ABS, ABS_MT_SLOT, slot)) return -1;  // ← 移除！
    if (send_event(fd, EV_ABS, ABS_MT_TRACKING_ID, ids->ids[slot])) return -1;
    return 0;
}

static int handle_down(int fd, struct touch_ids *ids, int slot, int x, int y, int orientation,
                      const real_device_info *info, int screen_width, int screen_height) {
    // 1. 设置 tracking_id（不发送 ABS_MT_SLOT）
    if (ensure_slot(fd, ids, slot)) return -1;

    // 2. 发送按键事件
    if (send_event(fd, EV_KEY, BTN_TOUCH, 1)) return -1;
    if (send_event(fd, EV_KEY, BTN_TOOL_FINGER, 1)) return -1;

    // 3. 根据屏幕方向转换坐标
    transform_coords(&x, &y, orientation, screen_width, screen_height);

    // 4. 添加坐标随机偏移（±0.5像素）
    double x_offset = get_coord_offset();
    double y_offset = get_coord_offset();
    int x_final = (int)((double)x + x_offset + 0.5);
    int y_final = (int)((double)y + y_offset + 0.5);

    // 确保坐标在有效范围内
    if (x_final < 0) x_final = 0;
    if (y_final < 0) y_final = 0;
    if (x_final > info->max_x) x_final = info->max_x;
    if (y_final > info->max_y) y_final = info->max_y;

    // 5. 发送坐标
    if (send_event(fd, EV_ABS, ABS_MT_POSITION_X, x_final)) return -1;
    if (send_event(fd, EV_ABS, ABS_MT_POSITION_Y, y_final)) return -1;

    // 6. 根据真实设备能力，动态发送相应事件
    if (info->has_touch_major) {
        int touch_value = get_random_touch_major();
        if (send_event(fd, EV_ABS, ABS_MT_TOUCH_MAJOR, touch_value)) return -1;
    }

    if (info->has_touch_minor) {
        int touch_value = get_random_touch_major();
        if (send_event(fd, EV_ABS, ABS_MT_TOUCH_MINOR, touch_value)) return -1;
    }

    if (info->has_orientation) {
        int touch_orientation = get_random_orientation();
        if (send_event(fd, EV_ABS, ABS_MT_ORIENTATION, touch_orientation)) return -1;
    }

    // 7. 同步
    return sync_report(fd);
}

static int handle_move(int fd, struct touch_ids *ids, int slot, int x, int y, int orientation,
                      const real_device_info *info, int screen_width, int screen_height) {
    // 真实设备 Move 事件：只发送坐标（+ 可选 TOUCH_MAJOR/MINOR），不发送 SLOT
    // 检查 slot 是否已经按下（必须有有效的 tracking_id）
    if (slot < 0 || slot >= DEFAULT_MAX_SLOTS) return -1;
    if (ids->ids[slot] < 0) return -1;  // slot 未按下，无法 move

    // 根据屏幕方向转换坐标
    transform_coords(&x, &y, orientation, screen_width, screen_height);

    // 添加坐标随机偏移（±0.5像素）
    double x_offset = get_coord_offset();
    double y_offset = get_coord_offset();
    int x_final = (int)((double)x + x_offset + 0.5);
    int y_final = (int)((double)y + y_offset + 0.5);

    // 确保坐标在有效范围内
    if (x_final < 0) x_final = 0;
    if (y_final < 0) y_final = 0;
    if (x_final > info->max_x) x_final = info->max_x;
    if (y_final > info->max_y) y_final = info->max_y;

    // 只发送坐标
    if (send_event(fd, EV_ABS, ABS_MT_POSITION_X, x_final)) return -1;
    if (send_event(fd, EV_ABS, ABS_MT_POSITION_Y, y_final)) return -1;

    // Move 时可选发送 TOUCH_MAJOR/MINOR（参考三星S23 getevent）
    if (info->has_touch_major) {
        int touch_value = get_random_touch_major();
        if (send_event(fd, EV_ABS, ABS_MT_TOUCH_MAJOR, touch_value)) return -1;
    }

    if (info->has_touch_minor) {
        int touch_value = get_random_touch_major();
        if (send_event(fd, EV_ABS, ABS_MT_TOUCH_MINOR, touch_value)) return -1;
    }

    return sync_report(fd);
}

static int handle_up(int fd, struct touch_ids *ids, int slot, int send_touch_key) {
    if (slot < 0 || slot >= DEFAULT_MAX_SLOTS) return -1;
    // 真实设备不发送 ABS_MT_SLOT
    // if (send_event(fd, EV_ABS, ABS_MT_SLOT, slot)) return -1;  // ← 移除！
    if (send_event(fd, EV_ABS, ABS_MT_TRACKING_ID, -1)) return -1;
    if (send_touch_key) {
        if (send_event(fd, EV_KEY, BTN_TOUCH, 0)) return -1;
        if (send_event(fd, EV_KEY, BTN_TOOL_FINGER, 0)) return -1;
    }
    ids->ids[slot] = -1;
    return sync_report(fd);
}

// Internal device structure
typedef struct {
    int fd;
    struct touch_ids ids;
    real_device_info real_info;  // 真实设备信息
    int screen_width;   // 屏幕宽度（竖屏）
    int screen_height;  // 屏幕高度（竖屏）
} touch_device;

// Global singleton device
static touch_device *g_device = NULL;

// Forward declaration
static int find_virtual_device_path(const char *device_name, char *out_path, size_t path_size);

// Initialize touch device (singleton)
// max_x, max_y: 触摸屏分辨率（必须提供）
int touch_init(int max_x, int max_y) {
    // 如果已经初始化，直接返回成功
    if (g_device != NULL) {
        return 0;
    }

    // 初始化随机数生成器
    init_touch_random();

    g_device = malloc(sizeof(touch_device));
    if (!g_device) return -1;

    // 保存屏幕尺寸（竖屏）
    g_device->screen_width = max_x;
    g_device->screen_height = max_y;

    // 查找并分析真实触摸屏设备（仅用于获取设备名称和能力特性）
    real_device_info real_info;
    if (find_real_touchscreen_info(&real_info) < 0) {
        // 如果找不到真实设备，使用默认值
        memset(&real_info, 0, sizeof(real_device_info));
        strncpy(real_info.name, DEFAULT_DEVICE_NAME, sizeof(real_info.name) - 1);
        real_info.has_touch_major = 1;
        real_info.max_touch_major = 255;
    }

    // 复制设备信息，但始终使用用户传入的分辨率作为虚拟设备的坐标范围
    g_device->real_info = real_info;
    g_device->real_info.max_x = max_x - 1;
    g_device->real_info.max_y = max_y - 1;

    // 使用配置好的信息创建虚拟设备
    g_device->fd = setup_device(&g_device->real_info, DEFAULT_MAX_SLOTS);
    if (g_device->fd < 0) {
        free(g_device);
        g_device = NULL;
        return -1;
    }

    for (int i = 0; i < DEFAULT_MAX_SLOTS; ++i) {
        g_device->ids.ids[i] = -1;
    }
    g_device->ids.next_id = 1;

    // 验证设备：发送测试事件，用 getevent 验证能否接收
    char device_path[256];
    if (find_virtual_device_path(g_device->real_info.name, device_path, sizeof(device_path)) < 0) {
        goto cleanup;
    }

    int pipefd[2];
    if (pipe(pipefd) < 0) {
        goto cleanup;
    }

    pid_t pid = fork();
    if (pid == 0) {
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[1]);
        execlp("getevent", "getevent", device_path, NULL);  // 不限制事件数量
        _exit(1);
    }
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        goto cleanup;
    }

    close(pipefd[1]);
    usleep(50 * 1000);  // 增加等待时间

    struct input_event ev = {0};
    gettimeofday(&ev.time, NULL);

    // 发送一个明显的事件：ABS_MT_TRACKING_ID
    ev.type = EV_ABS;
    ev.code = ABS_MT_TRACKING_ID;
    ev.value = 9999;
    write(g_device->fd, &ev, sizeof(ev));

    // 发送 SYN_REPORT
    ev.type = EV_SYN;
    ev.code = SYN_REPORT;
    ev.value = 0;
    write(g_device->fd, &ev, sizeof(ev));

    fd_set fds;
    struct timeval tv = {0, 100000};
    FD_ZERO(&fds);
    FD_SET(pipefd[0], &fds);
    int select_ret = select(pipefd[0] + 1, &fds, NULL, NULL, &tv);

    int verified = 0;
    if (select_ret > 0) {
        char buf[512];
        ssize_t n = read(pipefd[0], buf, sizeof(buf) - 1);
        if (n > 0) {
            buf[n] = '\0';
            verified = 1;
        }
    }

    close(pipefd[0]);
    kill(pid, SIGTERM);
    wait(NULL);

    if (!verified) {
        goto cleanup;
    }

    return 0;

cleanup:
    if (g_device->fd >= 0) {
        ioctl(g_device->fd, UI_DEV_DESTROY);
        close(g_device->fd);
    }
    free(g_device);
    g_device = NULL;
    return -1;
}

// Find the /dev/input/eventX path for our virtual device
static int find_virtual_device_path(const char *device_name, char *out_path, size_t path_size) {
    DIR *dir = opendir("/dev/input");
    if (!dir) return -1;

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strncmp(entry->d_name, "event", 5) != 0) continue;

        char path[256];
        snprintf(path, sizeof(path), "/dev/input/%s", entry->d_name);

        int fd = open(path, O_RDONLY);
        if (fd < 0) continue;

        char name[256] = {0};
        if (ioctl(fd, EVIOCGNAME(sizeof(name)), name) >= 0) {
            if (strcmp(name, device_name) == 0) {
                close(fd);
                closedir(dir);
                snprintf(out_path, path_size, "%s", path);
                return 0;
            }
        }
        close(fd);
    }

    closedir(dir);
    return -1;
}

// Touch down at position (压力、面积、方向自动随机)
// orientation: 0=竖屏, 1=横屏
int touch_down(int x, int y, int slot, int orientation) {
    if (!g_device) return -1;
    return handle_down(g_device->fd, &g_device->ids, slot, x, y, orientation,
                      &g_device->real_info, g_device->screen_width, g_device->screen_height);
}

// Move touch to position
// orientation: 0=竖屏, 1=横屏
int touch_move(int x, int y, int slot, int orientation) {
    if (!g_device) return -1;
    return handle_move(g_device->fd, &g_device->ids, slot, x, y, orientation,
                      &g_device->real_info, g_device->screen_width, g_device->screen_height);
}

// Release touch
int touch_up(int slot) {
    if (!g_device) return -1;
    return handle_up(g_device->fd, &g_device->ids, slot, 1);
}

// 直线滑动（从起点到终点）
// duration: 滑动持续时间（毫秒）
// orientation: 0=竖屏, 1=横屏
int touch_swipe(int start_x, int start_y, int end_x, int end_y, int duration, int slot, int orientation) {
    if (!g_device) return -1;

    // 按下起点
    if (touch_down(start_x, start_y, slot, orientation) < 0) return -1;

    // 计算移动距离和步数
    float distance_x = end_x - start_x;
    float distance_y = end_y - start_y;
    int steps = duration / 10;  // 每10ms一步
    if (steps < 1) steps = 1;

    float step_x = distance_x / steps;
    float step_y = distance_y / steps;
    long sleep_time_us = (duration * 1000) / steps;  // 微秒

    struct timeval start_time;
    gettimeofday(&start_time, NULL);

    // 移动
    for (int i = 1; i <= steps; i++) {
        // 计算下一步位置
        int next_x = start_x + (int)(step_x * i);
        int next_y = start_y + (int)(step_y * i);

        // 计算应该等待的时间
        struct timeval current_time;
        gettimeofday(&current_time, NULL);
        long elapsed_us = (current_time.tv_sec - start_time.tv_sec) * 1000000 +
                         (current_time.tv_usec - start_time.tv_usec);
        long target_time_us = i * sleep_time_us;
        long wait_us = target_time_us - elapsed_us;

        if (wait_us > 0) {
            usleep(wait_us);
        }

        if (touch_move(next_x, next_y, slot, orientation) < 0) return -1;
    }

    // 抬起
    return touch_up(slot);
}

// 贝塞尔曲线滑动（二次贝塞尔曲线）
// duration: 滑动持续时间（毫秒）
// orientation: 0=竖屏, 1=横屏
int touch_swipe_bezier(int start_x, int start_y, int end_x, int end_y, int duration, int slot, int orientation) {
    if (!g_device) return -1;

    // 按下起点
    if (touch_down(start_x, start_y, slot, orientation) < 0) return -1;

    // 计算步数
    int steps = duration / 10;  // 每10ms一步
    if (steps < 1) steps = 1;
    long sleep_time_us = (duration * 1000) / steps;

    // 计算贝塞尔控制点（添加随机偏移）
    float delta_x = end_x - start_x;
    float delta_y = end_y - start_y;
    float length = sqrtf(delta_x * delta_x + delta_y * delta_y);

    // 最大偏移量（垂直于移动方向）
    float max_offset = (length < 200 ? length : 200) * 0.4f;
    float angle = atan2f(delta_y, delta_x) + M_PI / 2;  // 垂直角度

    // 随机偏移（-0.5 ~ 0.5）
    float random_offset = (get_random_double() - 0.5f) * max_offset;

    // 控制点在中点附近
    float control_x = (start_x + end_x) / 2.0f + random_offset * cosf(angle);
    float control_y = (start_y + end_y) / 2.0f + random_offset * sinf(angle);

    struct timeval start_time;
    gettimeofday(&start_time, NULL);

    // 沿贝塞尔曲线移动
    for (int i = 1; i <= steps; i++) {
        float t = (float)i / steps;

        // 二次贝塞尔曲线公式
        float x = (1 - t) * (1 - t) * start_x +
                  2 * t * (1 - t) * control_x +
                  t * t * end_x;
        float y = (1 - t) * (1 - t) * start_y +
                  2 * t * (1 - t) * control_y +
                  t * t * end_y;

        // 时间控制
        struct timeval current_time;
        gettimeofday(&current_time, NULL);
        long elapsed_us = (current_time.tv_sec - start_time.tv_sec) * 1000000 +
                         (current_time.tv_usec - start_time.tv_usec);
        long target_time_us = i * sleep_time_us;
        long wait_us = target_time_us - elapsed_us;

        if (wait_us > 0) {
            usleep(wait_us);
        }

        if (touch_move((int)x, (int)y, slot, orientation) < 0) return -1;
    }

    // 抬起
    return touch_up(slot);
}

// Cleanup and destroy touch device
void touch_cleanup(void) {
    if (!g_device) return;
    if (g_device->fd >= 0) {
        ioctl(g_device->fd, UI_DEV_DESTROY);
        close(g_device->fd);
    }
    free(g_device);
    g_device = NULL;
}