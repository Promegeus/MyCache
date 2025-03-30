#pragma once 

#include <cstring>
#include <list>
#include <memory>
#include <mutex>
#include <unordered_map>

#include "KICachePolicy.h"

namespace MyCache
{

// 前向声明
template<typename Key, typename Value> class KLruCache;


//节点类
template<typename Key, typename Value>
class LruNode 
{
private:
    Key key_;
    Value value_;
    size_t accessCount_;  // 访问次数
    std::shared_ptr<LruNode<Key, Value>> prev_;     //前节点指针(智能指针)
    std::shared_ptr<LruNode<Key, Value>> next_;     //后节点指针

public:
    LruNode(Key key, Value value)
        : key_(key)
        , value_(value)
        , accessCount_(1) 
        , prev_(nullptr)
        , next_(nullptr)
    {}

    // 提供必要的访问器
    Key getKey() const { return key_; }
    Value getValue() const { return value_; }
    void setValue(const Value& value) { value_ = value; }
    size_t getAccessCount() const { return accessCount_; }
    void incrementAccessCount() { ++accessCount_; }

    friend class KLruCache<Key, Value>;     //允许缓存类直接操作节点
};


//缓存类
template<typename Key, typename Value>
class KLruCache : public KICachePolicy<Key, Value>
{
public:
    using LruNodeType = LruNode<Key, Value>;
    using NodePtr = std::shared_ptr<LruNodeType>;       //智能指针管理节点
    using NodeMap = std::unordered_map<Key, NodePtr>;   //哈希表快速查找

    KLruCache(int capacity)
        : capacity_(capacity)
    {
        initializeList();
    }

    ~KLruCache() override = default;

    // 添加缓存
    void put(Key key, Value value) override
    {
        if (capacity_ <= 0)
            return;
    
        std::lock_guard<std::mutex> lock(mutex_);   //加锁，保证线程安全
        auto it = nodeMap_.find(key);
        if (it != nodeMap_.end())
        {
            // 如果已经存在,则更新value,并调用get方法，代表该数据刚被访问
            updateExistingNode(it->second, value);
            return ;
        }

        addNewNode(key, value);
    }

    //通过参数获取value值
    bool get(Key key, Value& value) override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = nodeMap_.find(key);
        if (it != nodeMap_.end())
        {
            moveToMostRecent(it->second);
            value = it->second->getValue();
            return true;
        }
        return false;
    }

    //通过返回值获取value
    Value get(Key key) override
    {
        Value value{};
        // memset(&value, 0, sizeof(value));   // memset 是按字节设置内存的，对于复杂类型（如 string）使用 memset 可能会破坏对象的内部结构
        get(key, value);
        return value;
    }

    // 删除指定元素
    void remove(Key key) 
    {   
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = nodeMap_.find(key);
        if (it != nodeMap_.end())
        {
            removeNode(it->second);
            nodeMap_.erase(it);
        }
    }

    // 判断给定key的节点是否存在于缓存中
    bool contains(Key key)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = nodeMap_.find(key);
        if (it != nodeMap_.end())
            return true;
        else
            return false;
    }




private:
    // 初始化链表
    void initializeList()
    {
        // 创建首尾虚拟节点 避免处理空指针
        dummyHead_ = std::make_shared<LruNodeType>(Key(), Value());
        dummyTail_ = std::make_shared<LruNodeType>(Key(), Value());
        dummyHead_->next_ = dummyTail_;
        dummyTail_->prev_ = dummyHead_;
    }

    // 从链表尾部插入新结点
    void insertNode(NodePtr node) 
    {
        node->next_ = dummyTail_;
        node->prev_ = dummyTail_->prev_;
        dummyTail_->prev_->next_ = node;
        dummyTail_->prev_ = node;
    }

    // 移除节点
    void removeNode(NodePtr node) 
    {
        node->prev_->next_ = node->next_;
        node->next_->prev_ = node->prev_;
    }

    // 将该节点移动到链表尾，即更新到最新位置
    void moveToMostRecent(NodePtr node) 
    {
        removeNode(node);
        insertNode(node);
    }

    // 修改已经存在链表的节点，并更新至链表尾
    void updateExistingNode(NodePtr node, const Value& value) 
    {
        node->setValue(value);
        moveToMostRecent(node);
    }

    // 驱逐链表中最久未访问的，即链表头    最近最少访问
    void evictLeastRecent() 
    {
        NodePtr leastRecent = dummyHead_->next_;
        removeNode(leastRecent);    //从链表中移除
        nodeMap_.erase(leastRecent->getKey());  //从哈希表中移除
    }

    //添加一个新节点
    void addNewNode(const Key& key, const Value& value) 
    {
       if (nodeMap_.size() >= capacity_) 
       {
           evictLeastRecent();
       }

       NodePtr newNode = std::make_shared<LruNodeType>(key, value);
       insertNode(newNode);     //添加到链表
       nodeMap_[key] = newNode; //添加到哈希表
    }

    


private:
    int          capacity_;     // 缓存容量
    NodeMap      nodeMap_;      // 哈希表：键到节点的映射   key -> Node 
    std::mutex   mutex_;        // 保证线程安全
    NodePtr      dummyHead_;    // 虚拟头结点(永远在第一个节点之前)
    NodePtr      dummyTail_;    // 虚拟尾节点(永远在最后一个节点之后)
};






// LRU优化：Lru-k版本。 通过继承的方式进行再优化
template<typename Key, typename Value>
class KLruKCache : public KLruCache<Key, Value>
{
public:
    // 构造函数
    KLruKCache(int capacity, int historyCapacity, int k)
        : KLruCache<Key, Value>(capacity) // 调用基类构造
        , historyList_(std::make_unique<KLruCache<Key, size_t>>(historyCapacity))
        , k_(k)
    {}  

    //获取缓存
    bool get(Key key, Value& value)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        // 先判断是否存在于缓存中，如果存在则直接获取
        if (KLruCache<Key, Value>::contains(key))
        {
            return KLruCache<Key, Value>::get(key,value);
        }

        //获取历史记录中的访问次数，然后增加历史访问次数
        int historyCount = historyList_->get(key);
        historyList_->put(key, historyCount+1);

        // 如果数据历史访问次数达到上限，则添加入缓存
        if (historyCount+1 >= k_)
        {
            // 移除历史访问记录
            historyList_->remove(key);
            // 添加入缓存中
            KLruCache<Key, Value>::put(key, value);
        }

        // 如果刚加进缓存，就能访问到。否则就不在缓存中，所以无法命中，即在缓存中获取不到
        return KLruCache<Key, Value>::get(key,value);
    }

    Value get(Key key)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        Value value{};
        get(key, value);
        return value;
    }

    //添加缓存
    void put(Key key, Value value)
    {
        std::lock_guard<std::mutex> lock(mutex_);

        // 先判断是否存在于缓存中，如果存在于则直接覆盖
        if (KLruCache<Key, Value>::contains(key))
        {
            KLruCache<Key, Value>::put(key, value);
            return;
        }

        //获取历史记录中的访问次数，然后增加历史访问次数
        int historyCount = historyList_->get(key);
        historyList_->put(key, historyCount+1); 

        // 如果数据历史访问次数达到上限，则添加入缓存
        if (historyCount+1 >= k_)
        {
            // 移除历史访问记录
            historyList_->remove(key);
            // 添加入缓存中
            KLruCache<Key, Value>::put(key, value);
        }
    }


private:
    std::mutex   mutex_;
    int k_;     //在历史记录中访问K_次才可以进入缓存链表
    std::unique_ptr<KLruCache<Key, size_t>> historyList_; // 访问数据历史记录(value为访问次数)(即历史记录也用KLruCache存储)
    // 如果大容量长期运行的话 历史记录可以采用哈希表存储 定期清理哈希表中的低频数据
};







// lru优化：对lru进行分片，提高高并发使用的性能
template<typename Key, typename Value>
class KHashLruCaches: public KICachePolicy<Key, Value>
{
public:
    KHashLruCaches(size_t capacity, int sliceNum)
        : capacity_(capacity)
        , sliceNum_(sliceNum > 0 ? sliceNum : std::thread::hardware_concurrency())  //设置分片数量，若<=0则自动设为CPU核心数
    {
        size_t sliceSize = std::ceil(capacity / static_cast<double>(sliceNum_)); // 计算每个分片的容量(向上取整)(100分3片，每片34)
        for (int i = 0; i < sliceNum_; ++i)
        {
            // 创建分片缓存实例，每个分片是独立的LRU缓存
            lruSliceCaches_.emplace_back(new KLruCache<Key, Value>(sliceSize)); 
        }
    }

    void put(Key key, Value value)
    {
        // 获取key的hash值，通过hash值计算出对应的分片索引
        size_t sliceIndex = Hash(key) % sliceNum_;
        return lruSliceCaches_[sliceIndex]->put(key, value);
    }

    bool get(Key key, Value& value)
    {
        // 获取key的hash值，通过hash值计算出对应的分片索引
        size_t sliceIndex = Hash(key) % sliceNum_;
        return lruSliceCaches_[sliceIndex]->get(key, value);
    }

    Value get(Key key)
    {
        Value value;
        //memset(&value, 0, sizeof(value));
        get(key, value);
        return value;
    }



private:
    // 将key转换为对应hash值
    size_t Hash(Key key)
    {
        std::hash<Key> hashFunc;
        return hashFunc(key);
    }

private:
    size_t  capacity_;  // 总容量
    int     sliceNum_;  // 切片数量
    std::vector<std::unique_ptr<KLruCache<Key, Value>>> lruSliceCaches_; // 切片LRU缓存
};



} // namespace MyCache