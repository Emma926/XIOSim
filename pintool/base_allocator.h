/* A core allocator interface.
 *
 * Author: Sam Xi.
 */

#ifndef __ALLOCATOR_H__
#define __ALLOCATOR_H__

#include <map>
#include <sys/types.h>
#include <string>

namespace xiosim {

struct pid_cores_info {
  double current_penalty = 0;
  int num_cores_allocated = 0;
};

class BaseAllocator {

  protected:
    const double MARGINAL_SPEEDUP_THRESHOLD = 0.4;
    std::map<pid_t, pid_cores_info> *process_info_map;
    std::map<std::string, double*> *loop_speedup_map;
    int num_cores;

  public:
    BaseAllocator(int ncores);
    ~BaseAllocator();

    /* Returns the number of cores allotted to loop loop_name for its upcoming
     * parallel loop in process pid. If the loop is not found, num_cores_alloc
     * is set to -1.
     */
    virtual void AllocateCoresForLoop(
        char* loop_name, pid_t pid, int* num_cores_alloc) = 0;

    /* Parses a comma separated value file that contains predicted speedups for
     * each loop when run on 2,4,8,and 16 cores and stores the data in a map.
     */
    void LoadHelixSpeedupModelData(char* filepath);

    /* Interpolate speedup data points based on the 4 data points given in the
     * input files.
     */
    void InterpolateSpeedup(double* speedup_in, double* speedup_out);

    /* Returns a copy of the process data for pid in data. If pid does not exist
     * in the map, the data pointer is not modified. */
    void get_data_for_pid(pid_t pid, pid_cores_info* data);
};

}  // namespace xiosim

#endif
