#pragma once

#include <iomanip>
#include <map>
#include <cstring>
#include <ccronexpr.h>
#include <ctpl_stl.h>

#include "InterruptableSleep.h"

namespace Bosma {
    using Clock = std::chrono::system_clock;

    class Task {
    public:
        explicit Task(std::function<void()> &&f, int id = 0, bool recur = false, bool interval = false) :
	    f(std::move(f)), m_id{id}, recur(recur), interval(interval) {}

	virtual ~Task()
	{}

	int id()const noexcept
	{
	    return m_id;
	}
	
        virtual Clock::time_point get_new_time() const = 0;

        std::function<void()> f;
	int m_id;
        bool recur;
        bool interval;
    };

    class InTask : public Task {
    public:
        explicit InTask(std::function<void()> &&f, int id=0) : Task(std::move(f), id) {}

        // dummy time_point because it's not used
        Clock::time_point get_new_time() const override { return Clock::time_point(Clock::duration(0)); }
    };

    class EveryTask : public Task {
    public:
        EveryTask(Clock::duration time, std::function<void()> &&f, int id = 0, bool interval = false) :
	    Task(std::move(f), id, true, interval), time(time) {}

        Clock::time_point get_new_time() const override {
          return Clock::now() + time;
        };
        Clock::duration time;
    };

    class CCronTask : public Task {
    public:
        class BadCronExpression : public std::exception {
        public:
            explicit BadCronExpression(std::string msg) : msg_(std::move(msg)) {}

            const char *what() const noexcept override { return (msg_.c_str()); }

        private:
            std::string msg_;
        };

        CCronTask(std::string expression, std::function<void()> &&f, int id=0) :
	    Task(std::move(f), id, true),
	    exp(std::move(expression))
	{}

        Clock::time_point get_new_time() const override {
          cron_expr expr;
          memset(&expr, 0, sizeof(expr));
          const char *err = nullptr;
          cron_parse_expr(exp.c_str(), &expr, &err);
          if (err) {
            throw BadCronExpression(std::string(err));
          }
          time_t cur = Clock::to_time_t(Clock::now());
          time_t next = cron_next(&expr, cur);

          return Clock::from_time_t(next);
        };

        std::string exp;
    };

    inline bool try_parse(std::tm &tm, const std::string &expression, const std::string &format) {
      std::stringstream ss(expression);
      return !(ss >> std::get_time(&tm, format.c_str())).fail();
    }

    class Scheduler {
    public:
        explicit Scheduler(unsigned int max_n_tasks = 4) : done(false), threads(max_n_tasks + 1) {
          threads.push([this](int) {
              while (!done) {
                if (tasks.empty()) {
                  sleeper.sleep();
                } else {
                  auto time_of_first_task = (*tasks.begin()).first;
                  sleeper.sleep_until(time_of_first_task);
                }
                manage_tasks();
              }
          });
        }

        Scheduler(const Scheduler &) = delete;

        Scheduler(Scheduler &&) noexcept = delete;

        Scheduler &operator=(const Scheduler &) = delete;

        Scheduler &operator=(Scheduler &&) noexcept = delete;

        ~Scheduler() {
          done = true;
          sleeper.interrupt();
        }

        template<typename _Callable, typename... _Args>
        void in(const Clock::time_point time, _Callable &&f, int id, _Args &&... args) {
          std::shared_ptr<Task> t = std::make_shared<InTask>(
	      std::bind(std::forward<_Callable>(f), std::forward<_Args>(args)...), id);
          add_task(time, std::move(t));
        }

        template<typename _Callable, typename... _Args>
        void in(const Clock::duration time, _Callable &&f, int id, _Args &&... args) {
	    in(Clock::now() + time, std::forward<_Callable>(f), id, std::forward<_Args>(args)...);
        }

        template<typename _Callable, typename... _Args>
        void at(const std::string &time, _Callable &&f, int id, _Args &&... args) {
          // get current time as a tm object
          auto time_now = Clock::to_time_t(Clock::now());
          std::tm tm = *std::localtime(&time_now);

          // our final time as a time_point
          Clock::time_point tp;

          if (try_parse(tm, time, "%H:%M:%S")) {
            // convert tm back to time_t, then to a time_point and assign to final
            tp = Clock::from_time_t(std::mktime(&tm));

            // if we've already passed this time, the user will mean next day, so add a day.
            if (Clock::now() >= tp)
              tp += std::chrono::hours(24);
          } else if (try_parse(tm, time, "%Y-%m-%d %H:%M:%S")) {
            tp = Clock::from_time_t(std::mktime(&tm));
          } else if (try_parse(tm, time, "%Y/%m/%d %H:%M:%S")) {
            tp = Clock::from_time_t(std::mktime(&tm));
          } else {
            // could not parse time
            throw std::runtime_error("Cannot parse time string: " + time);
          }

          in(tp, std::forward<_Callable>(f), id, std::forward<_Args>(args)...);
        }

        template<typename _Callable, typename... _Args>
        void every(const Clock::duration time, _Callable &&f, int id, _Args &&... args) {
          std::shared_ptr<Task> t = std::make_shared<EveryTask>(time, std::bind(std::forward<_Callable>(f),
                                                                                std::forward<_Args>(args)...), id);
          auto next_time = t->get_new_time();
          add_task(next_time, std::move(t));
        }

        template<typename _Callable, typename... _Args>
        void ccron(const std::string &expression, _Callable &&f, int id, _Args &&... args) {
          std::shared_ptr<Task> t = std::make_shared<CCronTask>(expression, std::bind(std::forward<_Callable>(f),
                                                                                      std::forward<_Args>(args)...), id);
          auto next_time = t->get_new_time();
          add_task(next_time, std::move(t));
        }

        template<typename _Callable, typename... _Args>
        void interval(const Clock::duration time, _Callable &&f, int id, _Args &&... args) {
          std::shared_ptr<Task> t = std::make_shared<EveryTask>(time, std::bind(std::forward<_Callable>(f),
                                                                                std::forward<_Args>(args)...), id, true);
          add_task(Clock::now(), std::move(t));
        }

	void remove_task(int id)
	{
	    std::lock_guard<std::mutex> l{lock};
	    for(auto iter = tasks.begin(); iter != tasks.end(); )
	    {
		if(iter->second->id() == id)
		    iter = tasks.erase(iter);
		else
		    ++iter;
	    }
	    sleeper.interrupt();
	}

    private:
        std::atomic<bool> done;

        Bosma::InterruptableSleep sleeper;

        std::multimap<Clock::time_point, std::shared_ptr<Task>> tasks;
        std::mutex lock;
        ctpl::thread_pool threads;

        void add_task(const Clock::time_point time, std::shared_ptr<Task> t) {
          std::lock_guard<std::mutex> l(lock);
          tasks.emplace(time, std::move(t));
          sleeper.interrupt();
        }

        void manage_tasks() {
          std::lock_guard<std::mutex> l(lock);

          auto end_of_tasks_to_run = tasks.upper_bound(Clock::now());

          // if there are any tasks to be run and removed
          if (end_of_tasks_to_run != tasks.begin()) {
            // keep track of tasks that will be re-added
            decltype(tasks) recurred_tasks;

            // for all tasks that have been triggered
            for (auto i = tasks.begin(); i != end_of_tasks_to_run; ++i) {

              auto &task = (*i).second;

              if (task->interval) {
                // if it's an interval task, only add the task back after f() is completed
                threads.push([this, task](int) {
                    task->f();
                    // no risk of race-condition,
                    // add_task() will wait for manage_tasks() to release lock
                    add_task(task->get_new_time(), task);
                });
              } else {
                threads.push([task](int) {
                    task->f();
                });
                // calculate time of next run and add the new task to the tasks to be recurred
                if (task->recur)
                  recurred_tasks.emplace(task->get_new_time(), std::move(task));
              }
            }

            // remove the completed tasks
            tasks.erase(tasks.begin(), end_of_tasks_to_run);

            // re-add the tasks that are recurring
            for (auto &task : recurred_tasks)
              tasks.emplace(task.first, std::move(task.second));
          }
        }
    };
}
