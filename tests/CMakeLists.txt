cmake_minimum_required(VERSION 3.7)

# threads
# set(THREADS_PREFER_PTHREAD_FLAG ON)

add_executable(example example.cpp)
target_link_libraries(example PRIVATE
  Scheduler
  )

add_executable(test_ccronexpr test_ccronexpr.cpp)
target_link_libraries(test_ccronexpr PRIVATE
  Scheduler
  )

add_executable(test_task_remove test_task_remove.cpp)
target_link_libraries(test_task_remove PRIVATE
  Scheduler
  )
