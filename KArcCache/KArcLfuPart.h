#pragma once

#include "KArcCacheNode.h"
#include <unordered_map>
#include <map>
#include <mutex>

namespace MyCache 
{

template<typename Key, typename Value>
class ArcLfuPart 
{
//本代码中 主缓存的各链表和幽灵缓存链表均采用尾插法

public:
    using NodeType = ArcNode<Key, Value>;
    using NodePtr = std::shared_ptr<NodeType>;
    using NodeMap = std::unordered_map<Key, NodePtr>;
    using FreqMap = std::map<size_t, std::list<NodePtr>>;

    explicit ArcLfuPart(size_t capacity, size_t transformThreshold)
        : capacity_(capacity)
        , ghostCapacity_(capacity)
        , transformThreshold_(transformThreshold)
        , minFreq_(0)   //将最小频率初始化为0
    {
        initializeLists();
    }

    // 插入/更新缓存
    bool put(Key key, Value value) 
    {
        if (capacity_ == 0)     return false;    //容量为0，插入失败

        std::lock_guard<std::mutex> lock(mutex_);
        auto it = mainCache_.find(key);
        if (it != mainCache_.end()) 
        {
            return updateExistingNode(it->second, value);   //若缓存中已存在键为key的节点，更新现有节点
        }
        return addNewNode(key, value);  //若不存在，则插入新节点
    }

    // 获取缓存
    bool get(Key key, Value& value) 
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = mainCache_.find(key);
        if (it != mainCache_.end())     //若命中缓存
        {
            updateNodeFrequency(it->second);    //更新该节点频率
            value = it->second->getValue();     //获取value
            return true;
        }
        return false;   //未命中
    }

    // 检查幽灵缓存是否存在键为key的节点
    bool checkGhost(Key key, Value& ghostValue) 
    {
        auto it = ghostCache_.find(key);
        if (it != ghostCache_.end()) 
        {
            ghostValue = it->second->getValue();
            removeFromGhost(it->second);
            ghostCache_.erase(it);
            return true;
        }
        return false;
    }

    // 主缓存容量+1
    void increaseCapacity() { ++capacity_; }
    
    // 主缓存容量-1
    bool decreaseCapacity() 
    {
        if (capacity_ <= 0) return false;
        if (mainCache_.size() == capacity_) 
        {
            evictLeastFrequent();
        }
        --capacity_;
        return true;
    }

    // 检查主缓存中是否有键为key的节点
    bool existsInMain(Key key) 
    {
        return mainCache_.find(key) != mainCache_.end();
    }



private:
    // 初始化幽灵缓存链表结构
    void initializeLists() 
    {
        ghostHead_ = std::make_shared<NodeType>();
        ghostTail_ = std::make_shared<NodeType>();
        ghostHead_->next_ = ghostTail_;
        ghostTail_->prev_ = ghostHead_;
    }

    // 向幽灵缓存中添加一个节点
    void addToGhost(NodePtr node) 
    {
        //添加到链表中
        node->next_ = ghostTail_;
        node->prev_ = ghostTail_->prev_;
        ghostTail_->prev_->next_ = node;
        ghostTail_->prev_ = node;
        //添加到缓存中
        ghostCache_[node->getKey()] = node;
    }

    // 从幽灵链表中移除一个节点
    void removeFromGhost(NodePtr node) 
    {
        node->prev_->next_ = node->next_;
        node->next_->prev_ = node->prev_;
    }

    // 移除幽灵列表中最旧的节点
    void removeOldestGhost() 
    {
        NodePtr oldestGhost = ghostHead_->next_;
        if (oldestGhost != ghostTail_) 
        {
            removeFromGhost(oldestGhost);
            ghostCache_.erase(oldestGhost->getKey());
        }
    }

    // 驱逐主缓存中频率最低的节点中最久未访问的那个节点
    void evictLeastFrequent() 
    {
        if (freqMap_.empty()) return;   //缓存未空 不操作

        // 获取最小频率的列表
        auto& minFreqList = freqMap_[minFreq_];
        if (minFreqList.empty())  return;

        // 移除最少使用的节点
        NodePtr leastNode = minFreqList.front();
        minFreqList.pop_front();

        // 如果移除该节点后，该节点对应的频率列表为空，则删除该频率项
        if (minFreqList.empty()) 
        {
            freqMap_.erase(minFreq_);
            // 更新最小频率(不应直接+1，存在的频率不一定是连续的)
            if (!freqMap_.empty()) 
            {
                minFreq_ = freqMap_.begin()->first;     //map已排序，第一个即最小
            }
        }

        // 将节点移到幽灵缓存
        if (ghostCache_.size() >= ghostCapacity_) 
        {
            removeOldestGhost();
        }
        addToGhost(leastNode);
        
        // 从主缓存中移除
        mainCache_.erase(leastNode->getKey());
    }

    // 更新主缓存节点频率
    void updateNodeFrequency(NodePtr node) 
    {
        size_t oldFreq = node->getAccessCount();
        node->incrementAccessCount();
        size_t newFreq = node->getAccessCount();    //oldFreq+1

        // 把节点从旧频率列表中移除
        auto& oldList = freqMap_[oldFreq];
        oldList.remove(node);
        if (oldList.empty())    //如果该节点是原链表的最后一个节点，就把该频率项删除
        {
            freqMap_.erase(oldFreq);
            if (oldFreq == minFreq_)    //如果该节点恰好是最小频率链表的唯一一个节点，那就更新最小频率
            {
                minFreq_ = newFreq;     //能直接+1是因为该节点频次+1，所以更新后至少存在一个频率为oldFreq+1的节点
            }
        }

        // 添加到新频率列表
        if (freqMap_.find(newFreq) == freqMap_.end()) 
        {
            freqMap_[newFreq] = std::list<NodePtr>();   //如果新频率对应的链表不存在，先建链表
        }
        freqMap_[newFreq].push_back(node);
    }

    // 更新主缓存现有节点
    bool updateExistingNode(NodePtr node, const Value& value) 
    {
        node->setValue(value);  //更新value
        updateNodeFrequency(node);  //更新该节点频率
        return true;
    }

    // 向主缓存中添加一个节点
    bool addNewNode(const Key& key, const Value& value) 
    {
        if (mainCache_.size() >= capacity_) 
        {
            evictLeastFrequent();   //主缓存满了就先驱逐一个到幽灵缓存
        }

        NodePtr newNode = std::make_shared<NodeType>(key, value);
        mainCache_[key] = newNode;  //添加到主缓存hash表
        
        // 将新节点添加到频率为1的列表中
        if (freqMap_.find(1) == freqMap_.end()) 
        {
            freqMap_[1] = std::list<NodePtr>(); //如果不存在频率为1的链表就先建链表
        }
        freqMap_[1].push_back(newNode); //添加到链表中
        minFreq_ = 1;   //将最小频率设为1
        
        return true;
    }



    

private:
    size_t capacity_;           // 主缓存容量
    size_t ghostCapacity_;      // 幽灵缓存容量
    size_t transformThreshold_; // 转移阈值
    size_t minFreq_;            // 当前缓存中频率最小节点的频率
    std::mutex mutex_;          // 互斥锁

    NodeMap mainCache_;         // 主缓存hash表
    NodeMap ghostCache_;        // 幽灵缓存hash表
    FreqMap freqMap_;           // 频率->该频率对应的链表
    
    NodePtr ghostHead_;         // 幽灵列表虚拟头节点
    NodePtr ghostTail_;         // 幽灵列表虚拟尾节点
};

} // namespace MyCache