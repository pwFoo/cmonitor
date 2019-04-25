/*
 * njmon_cgroups.cpp -- interacts with Linux control groups to allow
 *                      njmon to monitor only the CPU/memory/disk resources
 *                      that the current cgroup allows to use.
 * Developer: Francesco Montorsi.
 * (C) Copyright 2018 Francesco Montorsi

 This program is free software: you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation, either version 3 of the License, or
 (at your option) any later version.

 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "njmon.h"
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <set>
#include <sstream>
#include <string.h>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <vector>

// ----------------------------------------------------------------------------------
// Constants
// ----------------------------------------------------------------------------------

#define MAX_LOGICAL_CPU (256)
#define MIN_ELAPSED_SECS (0.1)

// ----------------------------------------------------------------------------------
// C++ Helper functions
// ----------------------------------------------------------------------------------

/* static */
bool get_cgroup_path_for_pid(const std::string& cgroup_type, std::string& cgroup_pathOUT)
{
    /*
     *
     * ABOUT /proc/%d/cgroup:
     *   See http://man7.org/linux/man-pages/man7/cgroups.7.html, look for "/proc/[pid]/cgroup (since Linux 2.6.24)"
     *   Each line is composed by:
     *                     hierarchy-ID:controller-list:cgroup-path
     *   The problem is that this file does not provide you the FULL cgroup path, which depends on where exactly that
     * cgroup has been mounted.
     *
     * ABOUT /proc/%d/mounts:
     *   See http://man7.org/linux/man-pages/man5/fstab.5.html
     *   Each line is composed by:
     *                     fs_spec  fs_file  fs_vfstype  fs_mntops  fs_freq  fs_passno
     *   We are interested into the lines that indicate a specific CGROUP TYPE mountpoint like:
     *   under LXC:
     *     cgroup /sys/fs/cgroup/cpuset/lxc/eva-allinone-correlator-main cgroup rw,nosuid,nodev,noexec,relatime,cpuset 0
     * 0 under Docker: cgroup /sys/fs/cgroup/cpuset cgroup ro,nosuid,nodev,noexec,relatime,cpuset 0 0 the second string
     * fs_file (/sys/fs/cgroup/cpuset/lxc/eva-allinone-correlator-main or /sys/fs/cgroup/cpuset) tells you where to find
     * all the current value of that cgroup; the fourth string fs_mntops contains the indication of the cgroup type
     * (e.g. cpuset)
     */
    std::ifstream inputf("/proc/self/mounts");
    if (!inputf.is_open())
        return false; // cannot read the cgroup information!

    std::string line;
    while (std::getline(inputf, line)) {
        // cout << line << '\n';
        std::vector<std::string> tuple = split_string_in_array(line, ' ');
        if (tuple.size() != 6)
            return false; // invalid format

        std::string fs_spec = tuple[0];
        std::string fs_file = tuple[1];
        std::string fs_mntops = tuple[3];

        if (fs_spec == "cgroup" && fs_mntops.find(cgroup_type) != std::string::npos) {
            // found the right "cgroup type"

            if (fs_file.empty() || fs_file == "/") {
                // !!this process is NOT running under any cgroup!!
                cgroup_pathOUT = "";
                return false;
            } else {
                cgroup_pathOUT = fs_file;
                return true;
            }
        }
    }

    return false; // cgroup name not found
}

bool read_from_system_cpu_for_current_cgroup(std::string kernelPath, std::set<int>& cpus)
{
    std::set<int> empty_set;
    return read_integers_with_range_validation(kernelPath + "/cpuset.cpus", 0, INT32_MAX, cpus);
}

bool read_cpuacct_line(const std::string& path, std::vector<uint64_t>& valuesINT /* OUT */)
{
    static unsigned int num_cpus = 0;
    char line[8192];

    FILE* fp1 = 0;
    if ((fp1 = fopen(path.c_str(), "r")) == NULL)
        return false;

    if (fgets(line, 1000, fp1) == NULL) {
        fclose(fp1);
        return false;
    }

    fclose(fp1);

    std::vector<std::string> values = split_string_in_array(line, ' ');
    if (num_cpus == 0) {
        // first time we read the CPU stats
        num_cpus = values.size();
    } else {
        if (values.size() != num_cpus) {
            // error: we read a different number of CPUs compared to previous read
            num_cpus = 0;
            return false;
        }
    }

    valuesINT.resize(num_cpus);
    for (unsigned int i = 0; i < num_cpus; i++)
        string2int(values[i], valuesINT[i]);

    return true;
}

// ----------------------------------------------------------------------------------
// NjmonCollectorApp - Functions used by the njmon engine
// ----------------------------------------------------------------------------------

// GLOBALS

uint64_t cgroup_memory_limit_bytes = 0;
std::string cgroup_memory_kernel_path;
std::string cgroup_cpuacct_kernel_path;
std::string cgroup_cpuset_kernel_path;
std::set<int> cgroup_cpus;
int debug = 0;

// FUNCTIONS

void NjmonCollectorApp::cgroup_init()
{
    m_bCGroupsFound = 0;

    if (!get_cgroup_path_for_pid("memory", cgroup_memory_kernel_path)) {
        if (debug)
            printf("Could not find the 'memory' cgroup path. CGroup mode disabled.\n");
        return;
    }
    if (!get_cgroup_path_for_pid("cpu,cpuacct", cgroup_cpuacct_kernel_path)) {
        if (!get_cgroup_path_for_pid("cpuacct,cpu", cgroup_cpuacct_kernel_path)) {
            if (debug)
                printf("Could not find the 'cpuacct' cgroup path. CGroup mode disabled.\n");
            return;
        }
    }
    if (!get_cgroup_path_for_pid("cpuset", cgroup_cpuset_kernel_path)) {
        if (debug)
            printf("Could not find the 'cpuset' cgroup path. CGroup mode disabled.\n");
        return;
    }

    if (!read_integer(cgroup_memory_kernel_path + "/memory.limit_in_bytes", cgroup_memory_limit_bytes)) {
        if (debug)
            printf("Could not read the memory limit from 'memory' cgroup. CGroup mode disabled.\n");
        return;
    }
    if (!read_from_system_cpu_for_current_cgroup(cgroup_cpuset_kernel_path, cgroup_cpus)) {
        if (debug)
            printf("Could not read the CPUs from 'cpuset' cgroup. CGroup mode disabled.\n");
        return;
    }
    if (cgroup_memory_limit_bytes == 0) {
        if (debug)
            printf("Could not read the memory limit from 'memory' cgroup. CGroup mode disabled.\n");
        return;
    }

    // cpuset and memory cgroups found:
    m_bCGroupsFound = 1;
    if (debug) {
        printf("Found cpuset cgroup limiting to CPUs: %s\n", stl_container2string(cgroup_cpus, ",").c_str());
        printf("Found memory cgroup limiting to Bytes: %lu\n", cgroup_memory_limit_bytes);
    }
}

void NjmonCollectorApp::cgroup_config()
{
    if (m_bCGroupsFound == 0)
        return;

    psection("cgroup_config");
    pstring("memory_path", &cgroup_memory_kernel_path[0]);
    pstring("cpuacct_path", &cgroup_cpuacct_kernel_path[0]);
    pstring("cpuset_path", &cgroup_cpuset_kernel_path[0]);

    std::string tmp = stl_container2string(cgroup_cpus, ",");
    pstring("cpus", &tmp[0]);
    plong("memory_limit_bytes", cgroup_memory_limit_bytes);
    psectionend();
}

int NjmonCollectorApp::cgroup_is_allowed_cpu(int cpu)
{
    if (m_bCGroupsFound == 0)
        return 1; // allowed
    return cgroup_cpus.find(cpu) != cgroup_cpus.end();
}

void NjmonCollectorApp::cgroup_proc_memory()
{
    if (m_bCGroupsFound == 0)
        return;

    // See
    //   https://lwn.net/Articles/529927/
    //   https://www.kernel.org/doc/Documentation/cgroup-v1/memory.txt
    //   https://www.kernel.org/doc/Documentation/cgroup-v2.txt
    int i, len;
    uint64_t value;

    /* Static data */
    static FILE* fp = 0;
    static char line[8192];
    char label[512];

    if (fp == 0) {
        std::string path = cgroup_memory_kernel_path + "/memory.stat";
        if ((fp = fopen(path.c_str(), "r")) == NULL) {
            fp = 0;
            return;
        }
    } else
        rewind(fp);

    psection("cgroup_memory_stats");
    while (fgets(line, 1000, fp) != NULL) {
        len = strlen(line);
        if (strncmp(line, "total_", 6) != 0)
            continue; // skip NON-totals: collect only cgroup-total values

        for (i = 0; i < len; i++) {
            if (line[i] == '(')
                line[i] = '_';
            if (line[i] == ')')
                line[i] = ' ';
            if (line[i] == ':')
                line[i] = ' ';
            if (line[i] == '\n')
                line[i] = 0;
        }
        value = 0;
        sscanf(line, "%s %lu", label, &value);
        /*printf("read_data_numer(%s) |%s| |%s|=%lld\n", statname,label,numstr,atoll(numstr));*/
        plong(label, value);
    }

    if (read_integer(cgroup_memory_kernel_path + "/memory.failcnt", value))
        plong("failcnt", value);

    psectionend();
}

void NjmonCollectorApp::cgroup_proc_cpuacct(double elapsed_sec, bool print)
{
    if (m_bCGroupsFound == 0)
        return;

    /* NOTE: newer distros have stats like
     *
     *     /sys/fs/cgroup/cpu,cpuacct/cpuacct.usage_percpu_sys
     *     /sys/fs/cgroup/cpu,cpuacct/cpuacct.usage_percpu_user
     * but older ones (e.g. Centos7) have only:
     *     /sys/fs/cgroup/cpu,cpuacct/cpuacct.usage_percpu
     * Here we try to handle both cases.
     *
     * See:
     *  https://www.kernel.org/doc/Documentation/cgroup-v1/cpuacct.txt
     *  https://www.kernel.org/doc/Documentation/cgroup-v2.txt
     *  https://access.redhat.com/documentation/en-us/red_hat_enterprise_linux/6/html/resource_management_guide/sec-cpuacct
     */

    /* structure to recall previous values */
    struct cpuacct_utilisation {
        uint64_t counter_nsec_user_mode;
        uint64_t counter_nsec_sys_mode;
    };
    static struct cpuacct_utilisation prev_values[MAX_LOGICAL_CPU] = { 0 };

    // non-static data:
    char label[512];

    std::string path = cgroup_cpuacct_kernel_path + "/cpuacct.usage_percpu_sys";
    if (file_exists(path.c_str())) {

        // this system supports per-cpu system/user stats:

        std::vector<uint64_t> counter_nsec_sys_mode;
        if (!read_cpuacct_line(path, counter_nsec_sys_mode))
            return;

        std::vector<uint64_t> counter_nsec_user_mode;
        if (!read_cpuacct_line(cgroup_cpuacct_kernel_path + "/cpuacct.usage_percpu_user", counter_nsec_user_mode))
            return;

        if (counter_nsec_sys_mode.size() != counter_nsec_user_mode.size())
            return;
        if (counter_nsec_sys_mode.empty())
            return;

        if (debug)
            printf("Found cpuacct.usage_percpu_sys/user cgroups\n");

        if (print)
            psection("cgroup_cpuacct_stats");
        for (size_t i = 0; i < counter_nsec_user_mode.size(); i++) {

            /*
             * We know how much time has elapsed; we thus divide the delta
             * of the incremental counter of ns spent in user mode by the elapsed
             * to understand how much time (for this CPU) was spent in user mode.
             *
             * HOW TO TEST THIS CODE:
             * run
             *     make ; src/njmon_collector -C -c100 -s1 >test.json
             *     taskset --cpu-list 3 stress --cpu 1   # launch a "stress" process with CPU-affinity on cpu #3
             * then just verify that
             *     watch -n1 'grep cpu3 -A6 -B1 test.json | tail -20'
             * produces cpu3 at 100%
             */
            if (prev_values[i].counter_nsec_user_mode && prev_values[i].counter_nsec_sys_mode && print
                && elapsed_sec > MIN_ELAPSED_SECS) {
                double cpuUserPercent = // force newline
                    100 * ((double)(counter_nsec_user_mode[i] - prev_values[i].counter_nsec_user_mode))
                    / (elapsed_sec * 1E9);
                double cpuSysPercent = // force newline
                    100 * ((double)(counter_nsec_sys_mode[i] - prev_values[i].counter_nsec_sys_mode))
                    / (elapsed_sec * 1E9);

                // output JSON counter
                sprintf(label, "cpu%zu", i);
                psub(label);
                pdouble("user", cpuUserPercent);
                pdouble("sys", cpuSysPercent);
                psubend();
            }

            // save for next cycle
            prev_values[i].counter_nsec_user_mode = counter_nsec_user_mode[i];
            prev_values[i].counter_nsec_sys_mode = counter_nsec_sys_mode[i];
        }
        if (print)
            psectionend();
    } else {

        // just get the per-cpu total:

        std::vector<uint64_t> counter_nsec_user_mode;
        if (!read_cpuacct_line(cgroup_cpuacct_kernel_path + "/cpuacct.usage_percpu", counter_nsec_user_mode))
            return;
        if (counter_nsec_user_mode.empty())
            return;

        if (debug)
            printf("Reading data from cgroup cpuacct.usage_percpu");

        if (print)
            psection("cgroup_cpuacct_stats");
        for (size_t i = 0; i < counter_nsec_user_mode.size(); i++) {

            /*
             * Same comments for USER/SYS computations done above apply here!
             */
            if (prev_values[i].counter_nsec_user_mode && print && elapsed_sec > MIN_ELAPSED_SECS) {
                double cpuUserPercent = // force newline
                    100 * ((double)(counter_nsec_user_mode[i] - prev_values[i].counter_nsec_user_mode))
                    / (elapsed_sec * 1E9);

                // output JSON counter
                sprintf(label, "cpu%zu", i);
                psub(label);
                pdouble("user", cpuUserPercent);
                psubend();
            }

            // save for next cycle
            prev_values[i].counter_nsec_user_mode = counter_nsec_user_mode[i];
        }
        if (print)
            psectionend();
    }
}
