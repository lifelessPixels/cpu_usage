#include <linux/cpumask.h>
#include <linux/delay.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/kernel_stat.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/printk.h>
#include <linux/sched/cputime.h>
#include <linux/sched/loadavg.h>
#include <linux/smp.h>
#include <linux/types.h>
#include <linux/vmalloc.h>
#include <linux/workqueue.h>
#include <linux/workqueue_types.h>

#define PRINT_PREFIX "cpu_usage: "
#define cpu_usage_print_info(fmt, arg...) pr_info(PRINT_PREFIX fmt "\n", ##arg)
#define cpu_usage_print_error(fmt, arg...) pr_err(PRINT_PREFIX fmt "\n", ##arg)

static uint cpu_usage_report_period = 10;
module_param_named(period, cpu_usage_report_period, uint, 0);
MODULE_PARM_DESC(period, "Period (in seconds) at which the CPU usage will be reported");

// global module variables
static struct workqueue_struct* cpu_usage_report_work_queue = NULL;
static volatile bool should_work_still_run = true;
static unsigned long last_total_jiffies = 0;
static unsigned long last_relevant_jiffies = 0;

static void get_cpu_stats(unsigned long* output_total_jiffies, unsigned long* output_relevant_jiffies) {
    // get CPU stats in current moment
    int cpu_id;
    unsigned long total_jiffies = 0;
    unsigned long relevant_jiffies = 0;
    for_each_online_cpu(cpu_id) {
        struct kernel_cpustat cpu_stats;
        cpu_stats = kcpustat_cpu(cpu_id);

        // count all jiffies and the relevant ones
        for (int index = 0; index < NR_STATS; index++) {
            // calculate jiffies
            unsigned long current_stat = cpu_stats.cpustat[index];
            switch (index) {
                case CPUTIME_USER:
                case CPUTIME_NICE:
                case CPUTIME_SYSTEM:
                case CPUTIME_IRQ:
                case CPUTIME_SOFTIRQ:
                case CPUTIME_STEAL:
                    relevant_jiffies += current_stat;
            }

            // also count total jiffies
            total_jiffies += current_stat;
        }
    }

    *output_total_jiffies = total_jiffies;
    *output_relevant_jiffies = relevant_jiffies;
}

static void report_work_main(struct work_struct* work) {
    (void)(work);

    // fetch CPU stats
    unsigned long total_jiffies, relevant_jiffies;
    get_cpu_stats(&total_jiffies, &relevant_jiffies);
    unsigned long total_jiffies_diff = total_jiffies - last_total_jiffies;
    unsigned long relevant_jiffies_diff = relevant_jiffies - last_relevant_jiffies;

    // print usage info (in percent, hence * 100)
    // NOTE: differences between last measurement and current are used, because CPU stats are cumulative in kernel
    cpu_usage_print_info("average CPU load in last %u seconds (all cores averaged): %lu%%", cpu_usage_report_period,
                         (total_jiffies_diff > 0) ? (100 * relevant_jiffies_diff / total_jiffies_diff) : 0);

    // update last counters
    last_total_jiffies = total_jiffies;
    last_relevant_jiffies = relevant_jiffies;

    // reschedule itself if necessary
    if (should_work_still_run)
        queue_delayed_work(cpu_usage_report_work_queue, (struct delayed_work*)(work), HZ * cpu_usage_report_period);
}
DECLARE_DELAYED_WORK(cpu_usage_report_work, report_work_main);

static int __init cpu_usage_init(void) {
    // show info about module being initialized
    cpu_usage_print_info("enabled with period of %u seconds", cpu_usage_report_period);

    // create a workqueue for periodic report
    cpu_usage_report_work_queue = alloc_workqueue("cpu_usage", WQ_UNBOUND, 1);
    if (cpu_usage_report_work_queue == NULL) {
        cpu_usage_print_error("could not create a workqueue for periodic reports, aborting...");
        return -EAGAIN;
    }

    // fetch first jiffy stats
    get_cpu_stats(&last_total_jiffies, &last_relevant_jiffies);

    // queue periodic report work
    queue_delayed_work(cpu_usage_report_work_queue, &cpu_usage_report_work, HZ * cpu_usage_report_period);

    // report that the module was successfully loaded
    return 0;
}

static void __exit cpu_usage_exit(void) {
    // cancel running work, to avoid leaks and panics ;)
    should_work_still_run = false;
    cpu_usage_print_info("waiting for report work to end...");
    cancel_delayed_work_sync(&cpu_usage_report_work);

    // free work queue
    destroy_workqueue(cpu_usage_report_work_queue);

    // log info about module state change
    cpu_usage_print_info("disabled reporting and cleaned-up the module");
}

module_init(cpu_usage_init);
module_exit(cpu_usage_exit);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Karol Małecki (https://github.com/lifelessPixels");
MODULE_DESCRIPTION("Simple kernel module, that can periodically output CPU usage to dmesg");
