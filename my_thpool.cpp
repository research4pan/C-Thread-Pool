#include <iostream>
#include <pthread.h>
#include <queue>
#include <string>

// A refactored version of "https://github.com/Pithikos/C-Thread-Pool"

// ==================== Binary Semaphore Handler ====================
class SemaphoreHandler {
 public:
  SemaphoreHandler():sem_value_(0) {}
  SemaphoreHandler(int sem_value);

  int Reset();
  int GetQuotaAndPost();
  int GetQuotaAndPostAll();
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

int SemaphoreHandler::GetQuotaAndPostAll() {
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

  // Returns:
  //   Error code. 0 for normal.
  //
  // Args:
  //   job_ptr:      output, the obtained job.
  //   job_flag_ptr: output, false if fail to get a job.
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

JobQueue job_queue;

// void print(void* arg) {
//   long long value = reinterpret_cast<long long>(arg);
//   std::cout << value << std::endl;
// }

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

  return 0;
}
