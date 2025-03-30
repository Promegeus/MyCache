#pragma once

#include <cmath>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include "KICachePolicy.h"

namespace MyCache
{

// 前向声明
template<typename Key, typename Value> class KLfuCache;
template<typename Key, typename Value> class KLfuAgingCache;



// 频率链表
template<typename Key, typename Value>
class FreqList
{
private:
	//频率链表中的节点结构
    struct Node
    {
    	int freq;    //节点访问频率(次数)
		Key key;
		Value value;
		std::shared_ptr<Node> pre;  //前驱节点指针
		std::shared_ptr<Node> next;	//后继节点指针

		Node() 
		: freq(1), pre(nullptr), next(nullptr) {}
		Node(Key key, Value value) 
		: freq(1), key(key), value(value), pre(nullptr), next(nullptr) {}
    };

	using NodePtr = std::shared_ptr<Node>;
    int freq_; 		// 该链表对应的频率值
    NodePtr head_; 	// 假头结点
    NodePtr tail_; 	// 假尾结点


public:
	//构造函数，初始化指定频率的链表
	explicit FreqList(int n) 	//explicit 表示禁止隐性类型转换
     : freq_(n) 
    {
      head_ = std::make_shared<Node>();
      tail_ = std::make_shared<Node>();
      head_->next = tail_;
      tail_->pre = head_;
    }

	//判断链表是否为空
	bool isEmpty() const
	{
		return head_->next == tail_;
	}


	// 将节点添加到链表尾部(最近访问)
    void addNode(NodePtr node) 
	{
		if (!node || !head_ || !tail_) 
            return;

		node->pre = tail_->pre;
		node->next = tail_;
		tail_->pre->next = node;
		tail_->pre = node;
	}

	// 从链表中移除节点
	void removeNode(NodePtr node)
    {
        if (!node || !head_ || !tail_)
            return;
        if (!node->pre || !node->next) 
            return;

        node->pre->next = node->next;
        node->next->pre = node->pre;
        node->pre = nullptr;
        node->next = nullptr;
    }

	// 获取链表实际首节点(该链表中最久未被访问的节点)
	NodePtr getFirstNode() const 
	{ 
		return head_->next; 
	}

	//缓存类可以直接操作各频率链表
	friend class KLfuCache<Key, Value>;
	friend class KLfuAgingCache<Key, Value>;

    
};



// 缓存类
template <typename Key, typename Value>
class KLfuCache : public KICachePolicy<Key, Value>
{
public:
    using Node = typename FreqList<Key, Value>::Node;
    using NodePtr = std::shared_ptr<Node>;
    using NodeMap = std::unordered_map<Key, NodePtr>;

	//构造函数
	KLfuCache(int capacity)
    : capacity_(capacity), minFreq_(INT8_MAX)
    {}

	//析构函数
	~KLfuCache() override = default;

	//添加缓存 接口
	void put(Key key, Value value) override
	{
		if (capacity_ == 0)
            return;
		std::lock_guard<std::mutex> lock(mutex_);	//加锁，保证线程安全

		//先看看要添加的节点的key是否存在
		auto it = nodeMap_.find(key);
        if (it != nodeMap_.end())
		{
			//如果存在 把该节点的value值改为新节点的value值
			it->second->value = value;
			getInternal(it->second, value);
			return;
		}

		//如果不存在，就添加一个新的
		putInternal(key, value);
	}

	//获取缓存 接口 value值为传出参数
    bool get(Key key, Value& value) override
	{
		std::lock_guard<std::mutex> lock(mutex_);
		auto it = nodeMap_.find(key);
		if (it != nodeMap_.end())
		{
			getInternal(it->second, value);
			return true;
		}

		return false;
	}

	// 获取缓存 用返回值接收
	Value get(Key key) override
    {
		Value value;
		get(key, value);
		return value;
    }

	// 清空缓存,回收资源
    void purge()
    {
		nodeMap_.clear();
		freqToFreqList_.clear();
    }



protected:
	virtual void putInternal(Key key, Value value); // 添加缓存
	virtual void getInternal(NodePtr node, Value& value); // 获取缓存
	
    virtual void kickOut(); // 移除缓存中的过期数据

    void removeFromFreqList(NodePtr node); // 从频率列表中移除节点
    void addToFreqList(NodePtr node); // 把节点添加到对应频率列表(是添加缓存、获取缓存的一步)




protected:
	int capacity_;		//缓存容量
	int minFreq_;		//缓存中现存的最小访问频次(用于找到最小访问频次结点)
	std::mutex mutex_;	//互斥锁
	NodeMap nodeMap_;	//key 到缓存节点 的映射    key -> Node
	std::unordered_map<int, FreqList<Key,Value>*> freqToFreqList_;	// 访问频次 到该频次链表 的映射
};


// 把节点添加到对应频率列表 (是添加缓存、获取缓存的其中一步)
template<typename Key, typename Value>
void KLfuCache<Key, Value>::addToFreqList(NodePtr node)
{
	//检查是否是空节点
	if(!node)
		return;

	// 添加进入相应的频次链表前需要判断该频次链表是否存在
	if(freqToFreqList_.find(node->freq) == freqToFreqList_.end())
	{
		//如果不存在，先建立频次链表
		freqToFreqList_[node->freq] = new FreqList<Key, Value>(node->freq);
	}

	freqToFreqList_[node->freq]->addNode(node);
}

// 从某一节点的频率列表中移除该节点
template<typename Key, typename Value>
void KLfuCache<Key, Value>::removeFromFreqList(NodePtr node)
{
	if(!node)
		return;
	freqToFreqList_[node->freq]->removeNode(node);
};

// 移除缓存中的最不常使用的数据 (是添加缓存操作的一步)
template<typename Key, typename Value>
void KLfuCache<Key, Value>::kickOut()
{
	NodePtr node = freqToFreqList_[minFreq_]->getFirstNode();	//获取最低频次列表中最久未使用的节点指针
	removeFromFreqList(node);	//从该节点的频率列表中移除该节点
	nodeMap_.erase(node->key);	//从map中移除该节点
	//decreaseFreqNum(node->freq);
}

//获取缓存
template<typename Key, typename Value>
void KLfuCache<Key, Value>::getInternal(NodePtr node, Value& value)
{
	// 找到之后需要将其从低访问频次的链表中删除，并且添加到+1的访问频次链表中，
    // 访问频次+1, 然后把value值返回
    value = node->value;
	removeFromFreqList(node);
	node->freq++;
	addToFreqList(node);
	// 如果当前node的访问频次如果等于minFreq+1，并且其前驱链表为空，则说明
    // freqToFreqList_[node->freq - 1]链表因node的迁移已经空了，需要更新最小访问频次
    if (node->freq - 1 == minFreq_ && freqToFreqList_[node->freq - 1]->isEmpty())
        minFreq_++;

	// 总访问频次和当前平均访问频次都随之增加
    //addFreqNum();
}

//添加缓存
template<typename Key, typename Value>
void KLfuCache<Key, Value>::putInternal(Key key, Value value)
{
	//如果缓存满了，就驱逐一个缓存中最不常用的数据
	if(nodeMap_.size() == capacity_)
		kickOut();
	
	// 创建新结点，将新结点添加进入，更新最小访问频次
    NodePtr node = std::make_shared<Node>(key, value);
	//将节点添加至map和该节点相对应的频率链表(频率已被初始化为1)
	nodeMap_[key] = node;
	addToFreqList(node);
	//addFreqNum();
	minFreq_ = std::min(minFreq_,1);
}







//引入最大访问次数的优化版，通过继承的方式优化  (本代码：平均次数超过最大限定次数，则所有节点频次减最大值的一半)
template <typename Key, typename Value>
class KLfuAgingCache : public KLfuCache<Key, Value>
{
public:
	using Node = typename FreqList<Key, Value>::Node;
    using NodePtr = std::shared_ptr<Node>;
    using NodeMap = std::unordered_map<Key, NodePtr>;
    // 构造函数需要额外接收 maxAverageNum 参数
    KLfuAgingCache(int capacity, int maxAverageNum)
        : KLfuCache<Key, Value>(capacity),  // 调用基类构造函数
          maxAverageNum_(maxAverageNum),
          curTotalNum_(0),
          curAverageNum_(0) {}

protected:	//重写父类部分方法
    // 覆盖基类的 获取缓存方法，添加频率统计逻辑
    void getInternal(typename KLfuCache<Key, Value>::NodePtr node, Value& value) override
	{
        KLfuCache<Key, Value>::getInternal(node, value);
        addFreqNum();	// 更新访问次数统计
    }

    // 覆盖基类的 添加缓存方法，添加频率统计逻辑
    void putInternal(Key key, Value value) override
	{
        KLfuCache<Key, Value>::putInternal(key, value);  
        addFreqNum();	// 更新访问次数统计
    }

    // 覆盖淘汰逻辑，减少总访问次数
    void kickOut() override
	{
        auto node = this->freqToFreqList_[this->minFreq_]->getFirstNode();	// 先复制一份要淘汰的节点
        KLfuCache<Key, Value>::kickOut();	//移除该节点
        decreaseFreqNum(node->freq);		// 减少总访问次数（优化新增逻辑）
    }

private:
    void addFreqNum(); // 增加平均访问等频率
    void decreaseFreqNum(int num); // 减少平均访问等频率
    void handleOverMaxAverageNum(); // 处理当前平均访问频率超过上限的情况
    void updateMinFreq();

private:
    int maxAverageNum_;  // 最大允许的平均访问次数
    int curTotalNum_;    // 总访问次数
    int curAverageNum_;  // 当前平均访问次数
};


// 重新计算最小频次
template<typename Key, typename Value>
void KLfuAgingCache<Key, Value>::updateMinFreq() 
{
	this->minFreq_ = INT8_MAX;
	for (const auto& pair : this->freqToFreqList_) 
	{
		if(pair.second && !pair.second->isEmpty())	//该频次的链表存在，且不为空
		{
			this->minFreq_ = std::min(this->minFreq_, pair.first);
		}
	}
	if (this->minFreq_ == INT8_MAX) 
		this->minFreq_ = 1;
}

// 处理超过最大平均访问次数的情况
template<typename Key, typename Value>
void KLfuAgingCache<Key, Value>::handleOverMaxAverageNum()
{
	if(this->nodeMap_.empty())
		return;
	
	//遍历所有节点，降低其频次
	for(auto it = this->nodeMap_.begin(); it != this->nodeMap_.end(); it++)
	{
		// 检查结点是否为空
        if (!it->second)
            continue;
		
		NodePtr node = it->second;

		// 先从当前频次列表中移除
		this->removeFromFreqList(node);

		// 减少频次
		node->freq -= maxAverageNum_/2;
		if(node->freq < 1)
			node->freq = 1;
		
		//添加到新对应的频次链表
		this->addToFreqList(node);
	}

	//更新最小频次
	updateMinFreq();
}

// 增加总访问次数并检查是否触发降频
template<typename Key, typename Value>
void KLfuAgingCache<Key, Value>::addFreqNum()
{
	curTotalNum_++;
	if(this->nodeMap_.empty())
		curAverageNum_ = 0;
	else
		curAverageNum_ = curTotalNum_ / this->nodeMap_.size();
	
	// 若超过阈值，触发全局降频
	if(curAverageNum_ > maxAverageNum_)
	{
		handleOverMaxAverageNum();
	}
}

// 减少总访问次数（当节点被淘汰时调用）
template<typename Key, typename Value>
void KLfuAgingCache<Key, Value>::decreaseFreqNum(int num)
{
    // 减少平均访问频次和总访问频次
    curTotalNum_ -= num;
    if (this->nodeMap_.empty())
        curAverageNum_ = 0;
    else
        curAverageNum_ = curTotalNum_ / this->nodeMap_.size();
}









//分片优化
template<typename Key, typename Value>
class KHashLfuCache : public KICachePolicy<Key, Value>
{
public:
    KHashLfuCache(size_t capacity, int sliceNum, int maxAverageNum = 10)
        : sliceNum_(sliceNum > 0 ? sliceNum : std::thread::hardware_concurrency())
        , capacity_(capacity)
    {
        size_t sliceSize = std::ceil(capacity_ / static_cast<double>(sliceNum_)); // 每个lfu分片的容量
        for (int i = 0; i < sliceNum_; ++i)
        {
            lfuSliceCaches_.emplace_back(new KLfuAgingCache<Key, Value>(sliceSize, maxAverageNum));
        }
    }

    void put(Key key, Value value)
    {
        // 根据key找出对应的lfu分片
        size_t sliceIndex = Hash(key) % sliceNum_;
        return lfuSliceCaches_[sliceIndex]->put(key, value);
    }

    bool get(Key key, Value& value)
    {
        // 根据key找出对应的lfu分片
        size_t sliceIndex = Hash(key) % sliceNum_;
        return lfuSliceCaches_[sliceIndex]->get(key, value);
    }

    Value get(Key key)
    {
        Value value;
        get(key, value);
        return value;
    }

    // 清除缓存
    void purge()
    {
        for (auto& lfuSliceCache : lfuSliceCaches_)
        {
            lfuSliceCache->purge();
        }
    }

private:
    // 将key计算成对应哈希值
    size_t Hash(Key key)
    {
        std::hash<Key> hashFunc;
        return hashFunc(key);
    }

private:
    size_t capacity_; // 缓存总容量
    int sliceNum_; // 缓存分片数量
    std::vector<std::unique_ptr<KLfuAgingCache<Key, Value>>> lfuSliceCaches_; // 缓存lfu分片容器
};



}
// namespace MyCache

