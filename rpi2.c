#include "uart2.h"

static struct uart_regs __iomem *uart = NULL;
static void __iomem *gpio = NULL;
static struct proc_dir_entry *proc_tx;
static struct proc_dir_entry *proc_rx;
static struct proc_dir_entry *proc_config;
static struct proc_dir_entry *proc_status;
static struct proc_dir_entry *proc_stats;

// Configuration and statistics
static struct uart_config config = {
    .baudrate = BAUD_9600,
    .data_bits = DATA_BITS_8,
    .system_clock = 500000000  // 500MHz for RPi4
};

static struct uart_stats stats = {0};

// Mutex for thread-safe configuration changes
static DEFINE_MUTEX(uart_config_mutex);
static DEFINE_MUTEX(uart_tx_mutex);
static DEFINE_MUTEX(uart_rx_mutex);

// Delay function for GPIO setup - KEEP THIS, it's for very short hardware timing
static void delay_cycles(int count)
{
    while (count--) {
        cpu_relax();
    }
}

// Calculate baud rate register value
static int calculate_baud_register(u32 baudrate, u16 *reg_value)
{
    u32 denominator;
    u32 res;
    
    if (baudrate == 0 || baudrate > (config.system_clock / 8)) {
        pr_err("Invalid baud rate: %u\n", baudrate);
        return -EINVAL;
    }
    
    denominator = 8 * baudrate;
    res = (config.system_clock / denominator) - 1;
    
    if (res > 0xFFFF) {
        pr_err("Baud rate calculation overflow\n");
        return -EINVAL;
    }
    
    *reg_value = (u16)res;
    return 0;
}

// Clear FIFOs - CHANGED: usleep_range instead of udelay
static void uart_clear_fifos(void)
{
    // Clear RX FIFO
    writel(0x02, &uart->MU_IIR);
    // Clear TX FIFO  
    writel(0x04, &uart->MU_IIR);
    usleep_range(100, 150);  // CHANGED: was udelay(100)
}

// Apply current configuration to hardware
static int uart_apply_config(void)
{
    u16 baud_reg;
    
    if (calculate_baud_register(config.baudrate, &baud_reg) != 0) {
        return -EINVAL;
    }
    
    mutex_lock(&uart_config_mutex);
    
    // Disable TX/RX during reconfiguration
    writel(0x0, &uart->MU_CNTL);
    
    // Clear FIFOs
    uart_clear_fifos();
    
    // Set data bits (7 or 8 bit mode)
    writel(config.data_bits, &uart->MU_LCR);
    
    // Set baud rate
    writel(baud_reg, &uart->MU_BAUD);
    
    // Re-enable TX and RX
    writel(0x3, &uart->MU_CNTL);
    
    wmb();
    
    mutex_unlock(&uart_config_mutex);
    
    pr_info("UART reconfigured: baud=%u, data_bits=%s\n", 
            config.baudrate, 
            (config.data_bits == DATA_BITS_8) ? "8" : "7");
    
    return 0;
}

// Initialize Mini UART with GPIO configuration
static void uart_init_gpio(void)
{
    u32 val;
    void __iomem *gpfsel1;
    void __iomem *gppuppdn0;
    
    // Configure GPIO14 and GPIO15 for Mini UART (ALT5) 
    gpfsel1 = gpio + GPFSEL1;
    val = readl(gpfsel1);
    val &= ~((7 << 12) | (7 << 15));
    val |= (GPIO_FSEL_ALT5 << 12) | (GPIO_FSEL_ALT5 << 15);
    writel(val, gpfsel1);
    
    gppuppdn0 = gpio + GPPUPPDN0;
    val = readl(gppuppdn0);
    val &= ~((0x3 << 28) | (0x3 << 30));
    val |= (GPIO_PUPDN_NONE << 28) | (GPIO_PUPDN_UP << 30);
    writel(val, gppuppdn0);
    
    delay_cycles(150);  // KEEP THIS - hardware timing critical
}

// Initialize Mini UART hardware
static int uart_init_hardware(void)
{
    u32 val;
    u16 baud_reg;
    
    // Calculate initial baud rate
    if (calculate_baud_register(config.baudrate, &baud_reg) != 0) {
        return -EINVAL;
    }
    
    // Enable Mini UART in AUX enables register 
    val = readl(&uart->ENABLES);
    writel(val | 0x1, &uart->ENABLES);
    
    // Disable TX/RX during configuration
    writel(0x0, &uart->MU_CNTL);
    
    // Disable interrupts 
    writel(0x0, &uart->MU_IER);
    
    // Clear FIFOs
    uart_clear_fifos();
    
    // Set data format
    writel(config.data_bits, &uart->MU_LCR);
    
    // Disable flow control
    writel(0x0, &uart->MU_MCR);
    
    // Set baud rate
    writel(baud_reg, &uart->MU_BAUD);
    
    // Enable TX and RX 
    writel(0x3, &uart->MU_CNTL);
    
    wmb();
    
    pr_info("Mini UART initialized: baud=%u, data_bits=%s\n",
            config.baudrate,
            (config.data_bits == DATA_BITS_8) ? "8" : "7");
    
    return 0;
}

// Send a single char blocking - CHANGED: usleep_range for timeout
static void uart_send_char(char c)
{
    int timeout = 10000;  // 10ms total timeout
    
    // Handle newline 
    if (c == '\n') {
        uart_send_char('\r');
    }
    
    // Wait until TX FIFO has space with timeout
    // CHANGED: usleep_range instead of udelay
    while (!(readl(&uart->MU_LSR) & (1 << 5)) && timeout-- > 0) {
        usleep_range(1, 2);  // Sleep 1-2µs, much better than busy-wait
    }
    
    if (timeout <= 0) {
        stats.tx_errors++;
        pr_warn("TX timeout occurred\n");
        return;
    }
    
    // Write character to TX FIFO
    writel((u32)(c & 0xFF), &uart->MU_IO);
    stats.tx_bytes++;
}

// Send a string
static void uart_send_string(const char *s)
{
    mutex_lock(&uart_tx_mutex);
    
    while (*s) {
        if (*s == '\n') {
            uart_send_char('\r');
        }
        uart_send_char(*s++);
    }
    
    mutex_unlock(&uart_tx_mutex);
}

// Check if data is available to receive
static int uart_data_available(void)
{
    return (readl(&uart->MU_LSR) & (1 << 0));
}

// Check for receive errors
static void uart_check_rx_errors(void)
{
    u32 lsr = readl(&uart->MU_LSR);
    
    if (lsr & (1 << 1)) {  // Overrun error
        stats.fifo_overruns++;
        pr_warn("UART RX FIFO overrun detected\n");
    }
}

// Receive a single character (non-blocking)
static char uart_receive_char(void)
{
    if (!uart_data_available()) {
        return 0;
    }
    
    uart_check_rx_errors();
    stats.rx_bytes++;
    
    return (char)(readl(&uart->MU_IO) & 0xFF);
}

// Proc file read handler for receiving data
// CHANGED: usleep_range instead of udelay - HUGE performance improvement!
static ssize_t uart_proc_read(struct file *file, char __user *buf, 
                              size_t count, loff_t *ppos)
{
    char kbuf[512];
    int i = 0;
    char c;
    int consecutive_no_data = 0;
    const int MAX_CONSECUTIVE_NO_DATA = 300;
    
    if (*ppos > 0) {
        return 0;
    }
    
    mutex_lock(&uart_rx_mutex);
    
    // Wait for first character with timeout
    // CHANGED: usleep_range instead of udelay(1000)
    int timeout = 1000;  // 1 second total
    while (!uart_data_available() && timeout-- > 0) {
        usleep_range(1000, 1500);  // Sleep 1-1.5ms (was busy-waiting!)
    }
    
    if (timeout <= 0) {
        mutex_unlock(&uart_rx_mutex);
        return 0;
    }
    
    // Read all available characters
    while (i < (sizeof(kbuf) - 1) && i < count) {
        while (uart_data_available() && i < (sizeof(kbuf) - 1) && i < count) {
            c = uart_receive_char();
            if (c != 0) {
                kbuf[i++] = c;
                consecutive_no_data = 0;
            }
        }
        
        // CHANGED: usleep_range instead of udelay
        if (i > 0 && !uart_data_available()) {
            usleep_range(1000, 1500);  // Sleep 1-1.5ms
            consecutive_no_data++;
            
            if (consecutive_no_data >= MAX_CONSECUTIVE_NO_DATA) {
                break;
            }
        } else if (i == 0 && !uart_data_available()) {
            usleep_range(1000, 1500);  // Sleep 1-1.5ms
            consecutive_no_data++;
            if (consecutive_no_data >= MAX_CONSECUTIVE_NO_DATA) {
                break;
            }
        }
    }
    
    mutex_unlock(&uart_rx_mutex);
    
    if (i == 0) {
        return 0;
    }
    
    kbuf[i] = '\0';
    
    if (copy_to_user(buf, kbuf, i)) {
        stats.rx_errors++;
        return -EFAULT;
    }
    
    *ppos += i;
    
    pr_info("UART RX: received %d bytes\n", i);
    return i;
}

// Proc file write handler for transmitting data
static ssize_t uart_proc_write(struct file *file,
    const char __user *buf, size_t count, loff_t *ppos)
{
    char kbuf[512];
    size_t len;
    
    len = min(count, sizeof(kbuf) - 1);
    
    if (copy_from_user(kbuf, buf, len)) {
        stats.tx_errors++;
        return -EFAULT;
    }
    
    kbuf[len] = '\0';
    
    uart_send_string(kbuf);
    
    pr_info("UART TX: sent %zu bytes\n", len);
    
    return count;
}

// Configuration read handler
static ssize_t uart_config_read(struct file *file, char __user *buf,
                                size_t count, loff_t *ppos)
{
    char kbuf[512];
    int len;
    
    if (*ppos > 0) {
        return 0;
    }
    
    len = snprintf(kbuf, sizeof(kbuf),
        "UART Configuration\n"
        "==================\n"
        "Baudrate: %u\n"
        "Data bits: %s\n"
        "System clock: %u Hz\n"
        "\nSupported baud rates:\n"
        "  9600, 19200, 38400, 57600, 115200\n"
        "\nTo change configuration, write:\n"
        "  echo \"baud=115200\" > /proc/uart_config\n"
        "  echo \"bits=7\" > /proc/uart_config\n"
        "  echo \"clear_fifo\" > /proc/uart_config\n",
        config.baudrate,
        (config.data_bits == DATA_BITS_8) ? "8" : "7",
        config.system_clock);
    
    if (len > count) {
        len = count;
    }
    
    if (copy_to_user(buf, kbuf, len)) {
        return -EFAULT;
    }
    
    *ppos += len;
    return len;
}

// Configuration write handler
static ssize_t uart_config_write(struct file *file,
    const char __user *buf, size_t count, loff_t *ppos)
{
    char kbuf[128];
    size_t len;
    u32 new_baud;
    
    len = min(count, sizeof(kbuf) - 1);
    
    if (copy_from_user(kbuf, buf, len)) {
        return -EFAULT;
    }
    
    kbuf[len] = '\0';
    
    // Parse baud rate command
    if (sscanf(kbuf, "baud=%u", &new_baud) == 1) {
        // Validate baud rate
        if (new_baud != BAUD_9600 && new_baud != BAUD_19200 &&
            new_baud != BAUD_38400 && new_baud != BAUD_57600 &&
            new_baud != BAUD_115200) {
            pr_err("Unsupported baud rate: %u\n", new_baud);
            return -EINVAL;
        }
        
        config.baudrate = new_baud;
        if (uart_apply_config() != 0) {
            pr_err("Failed to apply baud rate configuration\n");
            return -EINVAL;
        }
        
        pr_info("Baud rate changed to %u\n", new_baud);
    }
    // Parse data bits command
    else if (strncmp(kbuf, "bits=8", 6) == 0) {
        config.data_bits = DATA_BITS_8;
        if (uart_apply_config() != 0) {
            return -EINVAL;
        }
        pr_info("Data bits changed to 8\n");
    }
    else if (strncmp(kbuf, "bits=7", 6) == 0) {
        config.data_bits = DATA_BITS_7;
        if (uart_apply_config() != 0) {
            return -EINVAL;
        }
        pr_info("Data bits changed to 7\n");
    }
    // Clear FIFO command
    else if (strncmp(kbuf, "clear_fifo", 10) == 0) {
        uart_clear_fifos();
        pr_info("FIFOs cleared\n");
    }
    // Reset statistics
    else if (strncmp(kbuf, "reset_stats", 11) == 0) {
        memset(&stats, 0, sizeof(stats));
        pr_info("Statistics reset\n");
    }
    else {
        pr_err("Invalid command. Use: baud=<rate>, bits=<7|8>, clear_fifo, or reset_stats\n");
        return -EINVAL;
    }
    
    return count;
}

// Status read handler
static ssize_t uart_status_read(struct file *file, char __user *buf,
                                size_t count, loff_t *ppos)
{
    char kbuf[512];
    int len;
    u32 lsr, stat;
    
    if (*ppos > 0) {
        return 0;
    }
    
    lsr = readl(&uart->MU_LSR);
    stat = readl(&uart->MU_STAT);
    
    len = snprintf(kbuf, sizeof(kbuf),
        "UART Status\n"
        "===========\n"
        "TX FIFO empty: %s\n"
        "TX FIFO full: %s\n"
        "RX FIFO has data: %s\n"
        "RX FIFO overrun: %s\n"
        "TX FIFO level: %u\n"
        "RX FIFO level: %u\n",
        (lsr & (1 << 5)) ? "Yes" : "No",
        (stat & (1 << 9)) ? "Yes" : "No",
        (lsr & (1 << 0)) ? "Yes" : "No",
        (lsr & (1 << 1)) ? "Yes (ERROR!)" : "No",
        (stat >> 24) & 0xF,
        (stat >> 16) & 0xF);
    
    if (len > count) {
        len = count;
    }
    
    if (copy_to_user(buf, kbuf, len)) {
        return -EFAULT;
    }
    
    *ppos += len;
    return len;
}

// Statistics read handler
static ssize_t uart_stats_read(struct file *file, char __user *buf,
                               size_t count, loff_t *ppos)
{
    char kbuf[512];
    int len;
    
    if (*ppos > 0) {
        return 0;
    }
    
    len = snprintf(kbuf, sizeof(kbuf),
        "UART Statistics\n"
        "===============\n"
        "TX bytes: %llu\n"
        "RX bytes: %llu\n"
        "TX errors: %llu\n"
        "RX errors: %llu\n"
        "FIFO overruns: %llu\n"
        "\nTo reset: echo \"reset_stats\" > /proc/uart_config\n",
        stats.tx_bytes,
        stats.rx_bytes,
        stats.tx_errors,
        stats.rx_errors,
        stats.fifo_overruns);
    
    if (len > count) {
        len = count;
    }
    
    if (copy_to_user(buf, kbuf, len)) {
        return -EFAULT;
    }
    
    *ppos += len;
    return len;
}

// Proc operations
static const struct proc_ops uart_tx_proc_ops = {
    .proc_write = uart_proc_write,
};

static const struct proc_ops uart_rx_proc_ops = {
    .proc_read = uart_proc_read,
};

static const struct proc_ops uart_config_proc_ops = {
    .proc_read = uart_config_read,
    .proc_write = uart_config_write,
};

static const struct proc_ops uart_status_proc_ops = {
    .proc_read = uart_status_read,
};

static const struct proc_ops uart_stats_proc_ops = {
    .proc_read = uart_stats_read,
};

// Module initialization 
static int __init uart_driver_init(void)
{
    int ret;
    
    // Map GPIO registers 
    gpio = ioremap(GPIO_BASE, 0x1000);
    if (!gpio) {
        pr_err("Failed to map GPIO registers\n");
        return -ENOMEM;
    }
    
    // Map UART registers 
    uart = ioremap(AUX_BASE, sizeof(struct uart_regs));
    if (!uart) {
        pr_err("Failed to map UART registers\n");
        iounmap(gpio);
        return -ENOMEM;
    }
    
    // Initialize GPIO
    uart_init_gpio();
    
    // Initialize UART hardware
    ret = uart_init_hardware();
    if (ret != 0) {
        pr_err("Failed to initialize UART hardware\n");
        iounmap(uart);
        iounmap(gpio);
        return ret;
    }
    
    // Create /proc/uart_tx
    proc_tx = proc_create(PROC_UART_TX, 0666, NULL, &uart_tx_proc_ops);
    if (!proc_tx) {
        pr_err("Failed to create /proc/%s\n", PROC_UART_TX);
        goto cleanup_uart;
    }
    
    // Create /proc/uart_rx
    proc_rx = proc_create(PROC_UART_RX, 0666, NULL, &uart_rx_proc_ops);
    if (!proc_rx) {
        pr_err("Failed to create /proc/%s\n", PROC_UART_RX);
        goto cleanup_tx;
    }
    
    // Create /proc/uart_config
    proc_config = proc_create(PROC_UART_CONFIG, 0666, NULL, &uart_config_proc_ops);
    if (!proc_config) {
        pr_err("Failed to create /proc/%s\n", PROC_UART_CONFIG);
        goto cleanup_rx;
    }
    
    // Create /proc/uart_status
    proc_status = proc_create(PROC_UART_STATUS, 0444, NULL, &uart_status_proc_ops);
    if (!proc_status) {
        pr_err("Failed to create /proc/%s\n", PROC_UART_STATUS);
        goto cleanup_config;
    }
    
    // Create /proc/uart_stats
    proc_stats = proc_create(PROC_UART_STATS, 0444, NULL, &uart_stats_proc_ops);
    if (!proc_stats) {
        pr_err("Failed to create /proc/%s\n", PROC_UART_STATS);
        goto cleanup_status;
    }
    
    // Send test message
    uart_send_string("Mini UART driver loaded successfully!\r\n");
    
    pr_info("===========================================\n");
    pr_info("UART driver loaded successfully\n");
    pr_info("Available /proc files:\n");
    pr_info("  /proc/%s - Write to send data\n", PROC_UART_TX);
    pr_info("  /proc/%s - Read to receive data\n", PROC_UART_RX);
    pr_info("  /proc/%s - Read/write configuration\n", PROC_UART_CONFIG);
    pr_info("  /proc/%s - Read current status\n", PROC_UART_STATUS);
    pr_info("  /proc/%s - Read statistics\n", PROC_UART_STATS);
    pr_info("===========================================\n");
    
    return 0;

cleanup_status:
    proc_remove(proc_status);
cleanup_config:
    proc_remove(proc_config);
cleanup_rx:
    proc_remove(proc_rx);
cleanup_tx:
    proc_remove(proc_tx);
cleanup_uart:
    iounmap(uart);
    iounmap(gpio);
    return -ENOMEM;
}

// Module cleanup
static void __exit uart_driver_exit(void)
{
    uart_send_string("Mini UART driver unloading...\r\n");
    
    // Remove all proc entries
    proc_remove(proc_stats);
    proc_remove(proc_status);
    proc_remove(proc_config);
    proc_remove(proc_rx);
    proc_remove(proc_tx);
    
    // Unmap registers
    if (uart)
        iounmap(uart);
    if (gpio)
        iounmap(gpio);
    
    pr_info("UART driver unloaded.\n");
}

module_init(uart_driver_init);
module_exit(uart_driver_exit);

MODULE_AUTHOR("Supriya Mishra");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("BCM2711 Mini UART Driver with Runtime Configuration");
