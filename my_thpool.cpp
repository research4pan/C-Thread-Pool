#define _POSIX_C_SOURCE 200809L
#include <ctime>
#include <iostream>
#include <pthread.h>
#include <queue>
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>

#if defined(__linux__)
#include <sys/prctl.h>
#endif

// A refactored version of "https://github.com/Pithikos/C-Thread-Pool"

// ==================== Binary Semaphore Handler ====================
class SemaphoreHandler {
 public:
  SemaphoreHandler():sem_value_(0) {}
  SemaphoreHandler(int sem_value);

  int Reset();
  int GetQuotaAndPost();
  int PostAll();
  int WaitForQuota();

 private:
  pthread_mutex_t mutex_;
  pthread_cond_t cond_;
  int sem_value_;
};


SemaphoreHandler::SemaphoreHandler(int sem_value) {
  if (sem_value < 0 || sem_value > 1) {
    std::cerr << "SemaphoreHandler(int sem_value): sem_value must be 0 or 1"
              << ": sem_value = " << sem_value
              << std::endl;
    exit(1);
  }
  pthread_mutex_init(&mutex_, NULL);
  pthread_cond_init(&cond_, NULL);
  sem_value_ = sem_value;
}


int SemaphoreHandler::Reset() {
  pthread_mutex_init(&mutex_, NULL);
  pthread_cond_init(&cond_, NULL);
  sem_value_ = 0;
  return 0;
}


int SemaphoreHandler::GetQuotaAndPost() {
  pthread_mutex_lock(&mutex_);
  sem_value_ = 1;
  pthread_cond_signal(&cond_);
  pthread_mutex_unlock(&mutex_);
  return 0;
}


int SemaphoreHandler::PostAll() {
  pthread_mutex_lock(&mutex_);
  sem_value_ = 1;
  pthread_cond_broadcast(&cond_);
  pthread_mutex_unlock(&mutex_);
  return 0;
}


int SemaphoreHandler::WaitForQuota() {
  pthread_mutex_lock(&mutex_);
  while (sem_value_ != 1) {
    pthread_cond_wait(&cond_, &mutex_);
  }
  sem_value_ = 0;
  pthread_mutex_unlock(&mutex_);
  return 0;
}


// ==================== Job and Job Queue ====================
struct Job {
  void (*function)(void* arg);      // The function a thread is going to execute
  void* arg;                        // The function's argument
};


class JobQueue {
 public:
  JobQueue();
  ~JobQueue();

  int Clear();
  int Push(const Job& job);

  // Gets a job from the queue's front and remove that job from the queue
  //
  // @param job_ptr:      output, the obtained job.
  // @param job_flag_ptr: output, false if fail to get a job.
  // @return 0 on success, -1 otherwise.
  int GetFrontAndPop(Job* job_ptr, bool* job_flag_ptr);

 public:
  int len_;                         // Number of jobs in queue
  SemaphoreHandler job_counter_;    // Counts available jobs in the queue and
                                    // notifies waiting threads

 private:
  pthread_mutex_t rwmutex_;         // Used in job queue's r/w access
  std::queue<Job> queue_;           // The queue itself
};


JobQueue::JobQueue(): len_(0), job_counter_(0) {
  pthread_mutex_init(&rwmutex_, NULL);
}


JobQueue::~JobQueue() {
  while (!queue_.empty()) {
    queue_.pop();
  }
}


int JobQueue::Clear() {
  pthread_mutex_lock(&rwmutex_);
  len_ = 0;
  job_counter_.Reset();
  while (!queue_.empty()) {
    queue_.pop();
  }
  pthread_mutex_unlock(&rwmutex_);
  return 0;
}


int JobQueue::Push(const Job& job) {
  pthread_mutex_lock(&rwmutex_);
  queue_.emplace(job);
  len_++;
  job_counter_.GetQuotaAndPost();
  pthread_mutex_unlock(&rwmutex_);
  return 0;
}


int JobQueue::GetFrontAndPop(Job* job_ptr, bool* job_flag_ptr) {
  pthread_mutex_lock(&rwmutex_);
  switch (len_) {
    case 0:     // No jobs in queue
      *job_flag_ptr = false;
      break;

    case 1:     // One job in queue, just satisfy this request
      *job_ptr = queue_.front();
      *job_flag_ptr = true;
      queue_.pop();
      len_ = 0;
      break;

    default:    // > 1 jobs in the queue, need ask other threads to work
      *job_ptr = queue_.front();
      *job_flag_ptr = true;
      queue_.pop();
      len_--;
      job_counter_.GetQuotaAndPost();
  }
  pthread_mutex_unlock(&rwmutex_);
  return 0;
}


// ==================== Thread and Thread Pool ====================
class Thread;
class ThreadPool;


class Thread {
 public:
  Thread(ThreadPool* thpool_ptr, int id);
  int Start();
  int Run();

  // Pthreads cannot handle member function directly because of the implicit
  // "this" argument, so this helper function is created to help
  static void* RunHelper(void* context);

 private:
  int id_;                         // Customized thread ID
  pthread_t pthread_;              // The actual pthread
  ThreadPool* thpool_ptr_;         // Access to the thread pool
};


class ThreadPool {
 public:
  ThreadPool(int num_threads);
  ~ThreadPool();
  int AddJob(void (*function_ptr)(void*), void* arg_ptr);
  int WaitForAllJobsFinished();       // Should be called AFTER adding all jobs!
                                      // Can serve as a barrier.

 public:
  volatile int num_threads_alive_;    // Volatile: tell compiler not to store
                                      // it in register/cache during compiler
                                      // optimization 
  volatile int num_threads_working_;
  volatile int threads_keepalive_;
                                      // Whether threads should to keep
                                      // running, even there is no job
  pthread_mutex_t mutex_;
  pthread_cond_t threads_all_idle_;   // A conditional variable which sends
                                      // signal when no thread is working
  JobQueue jobqueue_;
 private:
  std::vector<Thread> thread_vec_;
};


Thread::Thread(ThreadPool* thpool_ptr, int id) {
  thpool_ptr_ = thpool_ptr;
  id_ = id;
}

int Thread::Start() {
  // TODO ("&" need or not?)
  pthread_create(&pthread_,
                 NULL,
                 &Thread::RunHelper,
                 reinterpret_cast<void*>(this));

  // So no need to call pthread_join explicitly. It will have the same effect
  // once the thread exits or returns.
  pthread_detach(pthread_);
  return 0;
}

// Basically an endless loop (but CPU-friendly). It exits this loop only when
// thpool_destroy() is invoked or the program exits.
int Thread::Run() {
#if THPOOL_DEBUG
  printf("THPOOL_DEBUG: Thread #%d runs\n", id_);
  printf("THPOOL_DEBUG: Thread #%d thpool_ptr_ = %llu\n",
         id_, (unsigned long long)thpool_ptr_);
#endif

  // TODO (meaning of this statement?)
  // ThreadPool* thpool_ptr = thpool_ptr_;

  pthread_mutex_lock(&thpool_ptr_->mutex_);
  thpool_ptr_->num_threads_alive_++;
  pthread_mutex_unlock(&thpool_ptr_->mutex_);

#if THPOOL_DEBUG
  printf("THPOOL_DEBUG: Thread #%d alive\n", id_);
#endif

  // The main loop
  while (thpool_ptr_->threads_keepalive_) {

#if THPOOL_DEBUG
    printf("THPOOL_DEBUG: Thread #%d waits for job\n", id_);
#endif

    // Idle and wait
    thpool_ptr_->jobqueue_.job_counter_.WaitForQuota();

#if THPOOL_DEBUG
    printf("THPOOL_DEBUG: Thread #%d starts doing job\n", id_);
#endif

    if (thpool_ptr_->threads_keepalive_) {
      pthread_mutex_lock(&thpool_ptr_->mutex_);
      thpool_ptr_->num_threads_working_++;
      pthread_mutex_unlock(&thpool_ptr_->mutex_);
    }

    // Gets a job from queue and exeuctes
    Job job;
    bool job_flag = false;
    thpool_ptr_->jobqueue_.GetFrontAndPop(&job, &job_flag);
    if (job_flag) {
      job.function(job.arg);
    }

    pthread_mutex_lock(&thpool_ptr_->mutex_);
    thpool_ptr_->num_threads_working_--;
    if (!thpool_ptr_->num_threads_working_) {
      pthread_cond_signal(&thpool_ptr_->threads_all_idle_);
    }
    pthread_mutex_unlock(&thpool_ptr_->mutex_);
  }
  
  pthread_mutex_lock(&thpool_ptr_->mutex_);
  thpool_ptr_->num_threads_alive_--;
  pthread_mutex_unlock(&thpool_ptr_->mutex_);

  return 0;
}


void* Thread::RunHelper(void* context) {
  int ret = reinterpret_cast<Thread*>(context)->Run();
  return reinterpret_cast<void*>(ret);
}


ThreadPool::ThreadPool(int num_threads) {
  if (num_threads < 0) {
    num_threads = 0;
  }

  num_threads_alive_ = 0;
  num_threads_working_ = 0;
  threads_keepalive_ = 1;

  pthread_mutex_init(&mutex_, NULL);
  pthread_cond_init(&threads_all_idle_, NULL);

  // Allocates first, avoids vector from moving Thread elements to change their
  // addresses
  for (int thread_id = 0; thread_id < num_threads; thread_id++) {
    ThreadPool* thpool_ptr = this;
    thread_vec_.emplace_back(thpool_ptr, thread_id);
  }

  // Starts pthreads
  for (int thread_id = 0; thread_id < num_threads; thread_id++) {
    thread_vec_[thread_id].Start();
#if THPOOL_DEBUG
    // std::cout << "THPOOL_DEBUG: Created thread " << thread_id << " in pool"
    //          << std::endl;
    printf("THPOOL_DEBUG: Created thread %d in pool\n", thread_id);
#endif
  }

  // Busy-waiting until all threads finish initialization.
  // This is not the performance bottleneck, so it is okay to do so.
  while (num_threads_alive_ != num_threads) {}
}


ThreadPool::~ThreadPool() {
  volatile int num_threads = num_threads_alive_;

  // Ends each threads's endless while-loop
  threads_keepalive_ = 0;

  // After notifying all threads, they will wake up from idle and move on to
  // exit. Some threads may haven't reached the statement
  //
  //    `thpool_ptr_->jobqueue_.job_counter.WaitForQuota();`
  //
  // yet. This busy-waiting is to wait for those threads. However, in worst
  // cases, OS schedule may not give chance to those threads. So a follow-up
  // post is also necessary.
  double timeout = 1;
  time_t start, end;
  double time_passed = 0.0;
  time(&start);
  while (time_passed < timeout && num_threads_alive_) {
    jobqueue_.job_counter_.PostAll();
    time(&end);
    time_passed = difftime(end, start);
  }

  while (num_threads_alive_) {
    jobqueue_.job_counter_.PostAll();
    sleep(1);
  }

  // Destructors will automatically handle space deallocation
}


int ThreadPool::AddJob(void (*function_ptr)(void*), void* arg_ptr) {
  Job new_job;
  new_job.function = function_ptr;
  new_job.arg = arg_ptr;
  jobqueue_.Push(new_job);
  return 0;
}


int ThreadPool::WaitForAllJobsFinished() {
  pthread_mutex_lock(&mutex_);
  // Since the function is invoked after all jobs are added. If there is no
  // job in the queue, and no working thread, it implies all jobs have been
  // finished.
  while (jobqueue_.len_ || num_threads_working_) {
    // Notified by every thread who finishes its working job
    pthread_cond_wait(&threads_all_idle_, &mutex_);
  }
  pthread_mutex_unlock(&mutex_);
  return 0;
}

// JobQueue job_queue;

// void print(void* arg) {
//   long long value = reinterpret_cast<long long>(arg);
//   std::cout << value << std::endl;
// }

void task(void *arg){
  printf("Thread #%lld working on %lld\n",
         (long long)pthread_self(), (long long) arg);
}

int main() {
  // ===== Test for SemaphoreHandler
  // SemaphoreHandler sem_handler(0);
  // sem_handler.GetQuotaAndPost();
  // sem_handler.WaitForQuota();

  // ===== Test for JobQueue
  // Job job;
  // bool job_flag = false;
  // std::cout << job_flag << " " << reinterpret_cast<long long>(job.arg)
  //           << std::endl;

  // for (int i = 0; i < 10; i++) {
  //   Job job;
  //   job.function = *print;
  //   job.arg = reinterpret_cast<void*>(i);

  //   job_queue.Push(job);
  // }

  // while (job_queue.len_ > 0) {
  //   job_queue.GetFrontAndPop(&job, &job_flag);
  //   if (job_flag) {
  //     std::cout << job_flag << " " << reinterpret_cast<long long>(job.arg)
  //               << std::endl;
  //   }
  // }
  // job_queue.GetFrontAndPop(&job, &job_flag);
  // std::cout << job_flag << " " << reinterpret_cast<long long>(job.arg)
  //           << std::endl;

  // ===== Test for ThreadPool
	printf("Test starts\n");
  int num_threads = 2;
	ThreadPool thread_pool(num_threads);

	printf("Adding tasks to threadpool\n");
	for (int i = 0; i < 10; i++){
		thread_pool.AddJob(task, (void*)(uintptr_t)i);
	};
  thread_pool.WaitForAllJobsFinished();

	printf("Adding more tasks to threadpool\n");
	for (int i = 0; i < 10; i++){
		thread_pool.AddJob(task, (void*)(uintptr_t)i);
	};
  thread_pool.WaitForAllJobsFinished();
  return 0;
}
