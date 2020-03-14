
#include <cinttypes>
#include <cstdarg>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <iterator>
#include <string>

#include <fcntl.h>
#include <poll.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

    enum class Stream
    {
        STATUS = 0,
        STDIN,
        STDOUT,
        STDERR,
    };

    uint64_t outCmdCounter { }; // 64 bits should be enough for anyone?
    uint64_t clockBaseNs { };

    void print_usage()
    {
        puts("Usage: cuteseal-remote-runner <engine> [engine-options ...]\n"
             "\n"
             "Run engine and tag all input and output with time stamps. This is\n"
             "intended for lag elimination when running engines over a high-latency\n"
             "network.\n"
             "\n"
             "What this script essentially does is as follows:\n"
             "- Launches the engine\n"
             "- Per input inline:\n"
             "  o echo the received line to output with timing information attached\n"
             "  o pass the input to engine\n"
             "- Adds timing to output lines from the engine.\n"
             "This allows cutechess to do move time bookkeeping based on actual\n"
             "engine time use without the effects of the network lag.\n"
             "\n"
             "The input and output are line-buffered.\n"
             "\n"
             "The following format is used on the output:\n"
             "\n"
             "<line-num> <time-in-ns> <stream> LINE\n"
             "\n"
             "where: <line-num>   is a running line number (starts from 0)\n"
             "       <time-in-ns> is a nanosecond timestamp from a monotonic clock\n"
             "       <stream>     is one of: \"STATUS\", \"STDIN \" \"STDOUT\" \"STDERR\". Note\n"
             "                    the space in \"STDIN \". The streams are as one would expect:\n"
             "                    - STATUS is control messages from the remote runner.\n"
             "                    - STDIN is what is sent to the engine.\n"
             "                    - STDOUT is what the engine sends back in standard output.\n"
             "                    - STDERR is what the engine sends back in standard error.\n"
             "       LINE         is the line sent or received\n"
             "\n"
             "If line starts with 'cuteseal-deadline <ns>', then the runner will expect that\n"
             "the engine sends 'bestmove' command before the number of nanosecs has passed.\n"
             "If bestmove is not sent in time, the runner will send 'STATUS TIMEOUT' message,\n"
             "which the server-side will consider as a forfeit. This replaces the server-side\n"
             "timer-based timeout mechanism. The prefix 'cutechess-deadline <ns>' is not sent\n"
             "to the engine.\n");
    }

    uint64_t getClockNs()
    {
        constexpr uint64_t secsPerNs { 1000000000 };
        timespec tp { };
        clock_gettime(CLOCK_MONOTONIC, &tp);
        return (tp.tv_sec * secsPerNs + tp.tv_nsec) - clockBaseNs;
    }

    void timedPrintLine(Stream stream, const char *fmt, ...)
    {
        constexpr const char *streamNames[] { "STATUS", "STDIN ", "STDOUT", "STDERR" };

        va_list ap;
        const uint64_t ns { getClockNs() };

        printf("%" PRIu64 " %" PRIu64 " %s ",
               outCmdCounter,
               ns,
               streamNames[static_cast<size_t>(stream)]);

        va_start(ap, fmt);
        vprintf(fmt, ap);
        va_end(ap);

        puts(""); // newline

        outCmdCounter++;
    }

    void timedPerror(const char *str)
    {
        const char *error { strerror(errno) };

        timedPrintLine(Stream::STATUS, "%s: %s", str, error);
    }

    class FdLineBuffer
    {
    private:
        int fd;         // the fd to read data from
        int streamError { };
        char buf[4096]; // this is unprocessed data
        size_t bufpos { }; // position of the next unprocessed char
        size_t buflen { }; // length of current data

        std::string str; // this is the line string we're building

    public:
        FdLineBuffer(int in_fd) : fd(in_fd)
        {
            // we need the non-blocking mode
            int flags = fcntl(fd, F_GETFL);
            if (flags == -1) {
                streamError = errno;
            } else {
                flags = fcntl(fd, F_SETFL, flags | O_NONBLOCK);
                if (flags == -1) {
                    streamError = errno;
                }
            }
        }

        int getError() const
        {
            return streamError;
        }

        // return: true if line is available
        bool tryReadLine(std::string &line)
        {
            if (streamError) {
                return false;
            }

            while (true) {
                // get new data
                while (bufpos < buflen) {
                    const char c { buf[bufpos++] };

                    if (c == '\n') {
                        // end of string
                        line.clear();
                        std::swap(str, line);
                        return true;
                    }

                    str.push_back(c);
                }

                // ok, we got here, so try to read new data
                bufpos = 0;
                buflen = 0;
                const ssize_t rlen { read(fd, buf, sizeof buf) };
                if (rlen > 0) {
                    buflen = rlen;
                } else if (rlen == 0) {
                    streamError = ECONNRESET; // we'll use this to mark end of stream
                    return false;
                } else {
                    if (!(errno == EAGAIN || errno == EWOULDBLOCK)) {
                        // real error, and not just "no more data at the moment"
                        streamError = errno;
                    }
                    return false;
                }
            }
        }
    };

    void runLoop(int childStdin, int childStdout, int childStderr)
    {
        FdLineBuffer flbIn { STDIN_FILENO };
        FdLineBuffer flbOut { childStdout };
        FdLineBuffer flbErr { childStderr };
        bool allStreamsGood { true };
        FdLineBuffer *flbs[3] { &flbIn, &flbOut, &flbErr };
        uint64_t bestmoveDeadlineNs = 0; // positive if we have an active deadline

        FILE *toChild = fdopen(childStdin, "a");
        if (!toChild) {
            allStreamsGood = false;
            timedPerror("Failed to create child stdin file");
        }

        while (allStreamsGood) {
            pollfd fdsToPoll[3] { };
            fdsToPoll[0].fd = STDIN_FILENO;
            fdsToPoll[0].events = POLLIN | POLLRDHUP;
            fdsToPoll[1].fd = childStdout;
            fdsToPoll[1].events = POLLIN | POLLRDHUP;
            fdsToPoll[2].fd = childStderr;
            fdsToPoll[2].events = POLLIN | POLLRDHUP;

            // poll
            constexpr const char *pollEntryNames[std::size(fdsToPoll)] { "Input", "Engine output", "Engine stderr" };

            int pollDeadlineMs;

            if (bestmoveDeadlineNs == 0) {
                // no active DL
                pollDeadlineMs = -1;
            } else {
                // active DL
                int64_t nsLeft = getClockNs() - bestmoveDeadlineNs;
                if (nsLeft < 0) {
                    pollDeadlineMs = 0; // one more try, but no wait
                } else {
                    pollDeadlineMs = std::min<int>(std::numeric_limits<int>::max(), nsLeft / 1000000);
                }
            }

            if (poll(fdsToPoll, std::size(fdsToPoll), pollDeadlineMs) < 0) {
                timedPerror("Poll failed, aborting");
                abort();
            }

            // go through the streams
            std::string tmp;
            while (flbIn.tryReadLine(tmp)) {
                const char *line { tmp.c_str() };

                timedPrintLine(Stream::STDIN, "%s", line);

                if (strncmp("cuteseal-deadline ", line, 18) == 0) {
                    line += 18;
                    int chars = 0;
                    if (sscanf(line, "%" SCNd64 " %n", &bestmoveDeadlineNs, &chars) == 1)
                    {
                        line += chars;
                        bestmoveDeadlineNs += getClockNs(); // convert relative deadline to absolute dealine
                    }
                }

                // we'll also send this to the engine
                fputs(line, toChild);
                fputc('\n', toChild);
                fflush(toChild);
            }

            while (flbOut.tryReadLine(tmp)) {
                if (tmp.substr(0, 8) == "bestmove") {
                    // reset deadline
                    bestmoveDeadlineNs = 0;
                }

                timedPrintLine(Stream::STDOUT, "%s", tmp.c_str());
            }

            // deadline check
            if ((bestmoveDeadlineNs > 0) && (getClockNs() > bestmoveDeadlineNs)) {
                // timeout has been triggered
                timedPrintLine(Stream::STATUS, "TIMEOUT");
                bestmoveDeadlineNs = 0;
            }

            while (flbErr.tryReadLine(tmp)) {
                timedPrintLine(Stream::STDERR, "%s", tmp.c_str());
            }

            // check the streams for errors
            for (size_t i = 0; i < std::size(flbs); ++i) {
                if (flbs[i]->getError()) {
                    timedPrintLine(Stream::STATUS, "Stream %s has terminated: %s", pollEntryNames[i], strerror(flbs[i]->getError()));
                    allStreamsGood = false;
                    continue; // no need to spam the poll status
                }

                if (fdsToPoll[i].revents & (POLLHUP | POLLERR | POLLRDHUP)) {
                    timedPrintLine(Stream::STATUS, "Stream %s has terminated, poll status=%hd", pollEntryNames[i], fdsToPoll[i].revents);
                    allStreamsGood = false;
                }
            }
        }

        if (toChild) {
            fclose(toChild);
        }
    }

} // anonymous namespace

int main(int argc, char **argv)
{
    if (argc < 2) {
        print_usage();
        return 127;
    }

    // ensure we print in line-buffered mode
    setlinebuf(stdout);

    // reset our relative clock
    clockBaseNs = getClockNs();

    // set up the pipes and launch the engine; [0]=read end; [1]=write end
    int childIn[2] { -1, -1 };
    int childOut[2] { -1, -1 };
    int childErr[2] { -1, -1 };

    if (pipe2(childIn,  O_CLOEXEC)) {
        timedPerror("Failed to create STDIN for child");
        return 126;
    }

    if (pipe2(childOut, O_CLOEXEC)) {
        timedPerror("Failed to create STDOUT for child");
        return 126;
    }
    if (pipe2(childErr, O_CLOEXEC)) {
        timedPerror("Failed to create STDERR for child");
        return 126;
    }

    pid_t child = fork();
    if (child < 0) {
        timedPerror("Failed to create a child process");
        return 126;
    }

    if (child == 0) {
        // Note: these use intentionally perror(), as the fork parent will add
        // the timestamps to the output

        // rebind stdin/out/err - no cloexec for these
        if (dup2(childIn[0],  STDIN_FILENO) == -1) {
            perror("Failed to rebind STDIN for child");
            return 126;
        }
        if (dup2(childOut[1], STDOUT_FILENO) == -1)  {
            perror("Failed to rebind STDOUT for child");
            return 126;
        }
        if (dup2(childErr[1], STDERR_FILENO) == -1)  {
            perror("Failed to rebind STDERR for child");
            return 126;
        }

        // launch the engine
        execvp(argv[1], argv + 1);

        // if we get here, something went wrong
        perror("Failed to launch the engine");
        return 126;
    }

    // close the pipe ends that we don't need
    close(childIn[0]);
    close(childOut[1]);
    close(childErr[1]);

    timedPrintLine(Stream::STATUS, "Engine launched with pid %d with the following parameters", static_cast<int>(child));
    for (int i = 1; i < argc; ++i) {
        timedPrintLine(Stream::STATUS, "argv[%d]='%s'", (i - 1), argv[i]);
    }

    runLoop(childIn[1], childOut[0], childErr[0]);

    // exit from runLoop, make sure our child dies
    kill(child, SIGKILL);

    // wait for the child to terminate
    int wstatus { };
    if (waitpid(child, &wstatus, 0) == child) {
        if (WIFEXITED(wstatus)) {
            timedPrintLine(Stream::STATUS, "Engine has terminated with exit code %d", WEXITSTATUS(wstatus));
        } else if (WIFSIGNALED(wstatus)) {
            timedPrintLine(Stream::STATUS, "Engine has terminated by signal %d (%s)", WTERMSIG(wstatus), strsignal(WTERMSIG(wstatus)));
        } else {
            timedPrintLine(Stream::STATUS, "Engine terminated for unknown reason, waitpid status=%d", wstatus);
        }
    } else {
        timedPerror("Failed to wait for the child to terminate");
        return 126;
    }

    return 0;
}
