#pragma once

namespace starlet::os {

// Development profiler, configured by environment variables named after the port's prefix
// (for example "RE4" reads RE4_PROFILE):
//   <P>_PROFILE=<Hz>           samples each running thread's pc, lr and up to six frame-pointer
//                              return addresses on SIGPROF (process CPU time);
//   <P>_PROFILE_WALL           samples every thread at the rate instead, blocked ones included;
//   <P>_PROFILE_THREADS=a,b    limits wall-clock sampling to threads whose names start so;
//   <P>_PROFILE_FROM=<s>       drops samples before that time;
//   <P>_FRAME_STATS            prints a histogram of presentation intervals every 10 s.
// write_profile() writes <capture_dir>/profile.txt with thread names and executable mappings for
// symbolizing on the host.
void start_profiler(const char* prefix);
// Writes the samples; call on the exit paths. Does nothing when not profiling.
void write_profile(const char* capture_dir);
// Marks a presentation (frame marks in the profile, frame statistics); call where frames present.
void profile_frame();

}
