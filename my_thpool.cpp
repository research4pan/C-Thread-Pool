#include <iostream>
#include <pthread.h>
#include <string>

// A refactored version of "https://github.com/Pithikos/C-Thread-Pool"

// In fact, just a binary semaphore handler: the semaphore value can only be
// 0 or 1
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


int main() {
  // TODO
  SemaphoreHandler sem_handler(0);
  // sem_handler.GetQuotaAndPost();
  sem_handler.WaitForQuota();
  return 0;
}
