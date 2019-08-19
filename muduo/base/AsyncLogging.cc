#include <muduo/base/AsyncLogging.h>
#include <muduo/base/LogFile.h>
#include <muduo/base/Timestamp.h>

#include <stdio.h>

using namespace muduo;

AsyncLogging::AsyncLogging(const string& basename,
                           size_t rollSize,
                           int flushInterval)
  : flushInterval_(flushInterval),
    running_(false),
    basename_(basename),
    rollSize_(rollSize),
    thread_(boost::bind(&AsyncLogging::threadFunc, this), "AsyncLogging"),
    latch_(1),
    mutex_(),
    cond_(mutex_),
    currentBuffer_(new Buffer),
    nextBuffer_(new Buffer),
    buffers_()
{
  currentBuffer_->bzero();
  nextBuffer_->bzero();
  buffers_.reserve(16);
}

void AsyncLogging::append(const char* logline, int len)
{
  muduo::MutexLockGuard lock(mutex_); //注意：在生产者写入消费者的缓冲区时要保证线程安全
  if (currentBuffer_->avail() > len)
  {
    // 当前缓冲区未满，将数据追加到末尾
    currentBuffer_->append(logline, len);
  }
  else
  {
    // 当前缓冲区已满，将当前缓冲区添加到待写入文件的已填满的缓冲区列表
    buffers_.push_back(currentBuffer_.release());

    // 将当前缓冲区设置为预备缓冲区
    if (nextBuffer_)
    {
      currentBuffer_ = boost::ptr_container::move(nextBuffer_); // 移动语义
    }
    else
    {
      // 这种情况，极少发生，前端写入速度太快，一下子把两块缓冲区都写完，
	  // 那么，只好分配一块新的缓冲区。
      currentBuffer_.reset(new Buffer); // Rarely happens
    }
    currentBuffer_->append(logline, len);
    cond_.notify(); // 通知后端开始写入日志
  }
}

void AsyncLogging::threadFunc()
{
  assert(running_ == true);
  latch_.countDown();  //后台线程启动完成，通知前台线程继续运行
  
  LogFile output(basename_, rollSize_, false, flushInterval_); // 写入到日志文件时不用保证线程安全，
  															   // 因为只有一个后台线程进行写入
  // 准备两块空闲缓冲区
  BufferPtr newBuffer1(new Buffer);
  BufferPtr newBuffer2(new Buffer);
  newBuffer1->bzero();
  newBuffer2->bzero();
  BufferVector buffersToWrite; // 用于写入文件时暂存buffers_的内容，减小临界区的长度
  buffersToWrite.reserve(16);
  
  while (running_)
  {
    assert(newBuffer1 && newBuffer1->length() == 0);
    assert(newBuffer2 && newBuffer2->length() == 0);
    assert(buffersToWrite.empty());

    {
      muduo::MutexLockGuard lock(mutex_);

// 优化位置1：减少所做的无用功------------------------------------------------------------------------------------------
// 在原来的写法中，如果在flushInterval_时间内都没有生产者进行写入，那么后端线程将进行空转并做了大量的无用功

	  //if (buffers_.empty())  // unusual usage!（注意，这里是一个非常规用法）
	  //{
	  //  cond_.waitForSeconds(flushInterval_); // 等待前端写满了一个或者多个buffer,或者一个超时时间到来
	  //}

	  while (buffers_.empty() && running_)
      {
        cond_.waitForSeconds(flushInterval_); // 等待前端写满了一个或者多个buffer,或者一个超时时间到来
        if (currentBuffer_->length() > 0) {   // 如果3s之内写入了一些数据到缓冲区但又未写满，应及时写入文件
			break;
		}
      }
//----------------------------------------------------------------------------------------------------------------------

      buffers_.push_back(currentBuffer_.release()); // 将当前缓冲区移入buffers_
      currentBuffer_ = boost::ptr_container::move(newBuffer1); // 将空闲的newBuffer1置为当前缓冲区
      buffersToWrite.swap(buffers_); // buffers_与buffersToWrite交换，这样后面的代码可以在临界区之外安全地访问buffersToWrite
      if (!nextBuffer_)
      {
        nextBuffer_ = boost::ptr_container::move(newBuffer2); // 确保前端始终有一个预备buffer可供调配，减少前端
                                                              // 临界区分配内存的概率，缩短前端临界区长度。
      }
    }

    assert(!buffersToWrite.empty());

    // 消息堆积
    // 前端陷入死循环，拼命发送日志消息，超过后端的处理能力，这就是典型的生产速度
    // 超过消费速度问题，会造成数据在内存中堆积，严重时引发性能问题（可用内存不足）
    // 或程序崩溃（分配内存失败）
    if (buffersToWrite.size() > 25)
    {
      char buf[256];
      snprintf(buf, sizeof buf, "Dropped log messages at %s, %zd larger buffers\n",
               Timestamp::now().toFormattedString().c_str(),
               buffersToWrite.size()-2);
      fputs(buf, stderr);
      output.append(buf, static_cast<int>(strlen(buf)));
      buffersToWrite.erase(buffersToWrite.begin()+2, buffersToWrite.end()); // 丢掉多余日志，以腾出内存，仅保留两块缓冲区
	  //这里会自动释放new出来的Buffer
    }

    for (size_t i = 0; i < buffersToWrite.size(); ++i)
    {
      // FIXME: use unbuffered stdio FILE ? or use ::writev ?
      output.append(buffersToWrite[i].data(), buffersToWrite[i].length());
    }

    if (buffersToWrite.size() > 2)
    {
      // drop non-bzero-ed buffers, avoid trashing
      buffersToWrite.resize(2); // 仅保存两个buffer，用于newBuffer1与newBuffer2
    }

    if (!newBuffer1)
    {
      assert(!buffersToWrite.empty());
      newBuffer1 = buffersToWrite.pop_back();
      newBuffer1->reset(); //注意缓冲区用过之后要调用FixedBuffer.reset()重设缓冲区游标
    }

    if (!newBuffer2)
    {
      assert(!buffersToWrite.empty());
      newBuffer2 = buffersToWrite.pop_back();
      newBuffer2->reset(); //注意缓冲区用过之后要调用FixedBuffer.reset()重设缓冲区游标
    }

    buffersToWrite.clear();
    output.flush();
  }
  output.flush();
}

