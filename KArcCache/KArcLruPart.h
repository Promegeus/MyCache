#pragma once

#include "KArcCacheNode.h"
#include <unordered_map>
#include <mutex>

namespace MyCache 
{

template<typename Key, typename Value>
class ArcLruPart 
{
// 本代码缓存链表采用头插法，KLruCache.h中采用尾插法，没有什么本质不同

public:
    using NodeType = ArcNode<Key, Value>;   //缓存节点类型
    using NodePtr = std::shared_ptr<NodeType>;      //缓存节点指针类型
    using NodeMap = std::unordered_map<Key, NodePtr>;       //缓存hash表 ( key->节点指针 )

    // 构造函数：初始化容量、幽灵缓存容量和转移阈值
    explicit ArcLruPart(size_t capacity, size_t transformThreshold)
        : capacity_(capacity)
        , ghostCapacity_(capacity)
        , transformThreshold_(transformThreshold)
    {
        initializeLists();  // 初始化主缓存双向链表、幽灵缓存双向链表
    }

    // 在主缓存中 插入/更新缓存项
    bool put(Key key, Value value) 
    {
        if (capacity_ == 0) return false;   //主缓存容量若为0 直接返回
        
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = mainCache_.find(key);
        if (it != mainCache_.end()) 
        {
            //如果在主缓存hash表中找到了，说明已经在主缓存里了，执行更新现有节点操作(更新value，移至链表头部)
            //优化：并记录是否达到阈值，只有达到转移阈值时返回true (这在KArcCache.h中的put会用到)
            bool shouldPromote = updateNodeAccess(it->second);
            it->second->setValue(value);
            return shouldPromote;
        }
        //如果在主缓存hash表中找不到，说明不在主缓存里，执行添加新节点操作
        return addNewNode(key, value);
        return false;
    }

    // 在主缓存中 获取缓存项，并将是否转移lfu 通过参数的方式返回
    bool get(Key key, Value& value, bool& shouldTransform) 
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = mainCache_.find(key);
        if (it != mainCache_.end()) 
        {   // 命中主缓存
            //更新节点访问状态(移动至链表头部，增加节点访问计数)，并将 是否转移lfu 通过参数的方式返回
            shouldTransform = updateNodeAccess(it->second); 
            value = it->second->getValue(); //获取值
            return true;
        }
        return false;   //未命中主缓存
    }

    // 检查幽灵缓存中是否存在键为key的节点
    bool checkGhost(Key key, Value& ghostValue) 
    {
        auto it = ghostCache_.find(key);
        if (it != ghostCache_.end()) 
        {
            // 如果存在，就移除，并返回gValue
            ghostValue = it->second->getValue();
            removeFromGhost(it->second);
            ghostCache_.erase(it);
            return true;
        }
        return false;
    }

    //增加主缓存容量
    void increaseCapacity() { ++capacity_; }
    
    //减少主缓存容量
    bool decreaseCapacity() 
    {
        if (capacity_ <= 0) return false;
        if (mainCache_.size() == capacity_) 
        {
            evictLeastRecent();
        }
        --capacity_;
        return true;
    }

    // 从主缓存中删除指定元素
    void remove(Key key) 
    {   
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = mainCache_.find(key);
        if (it != mainCache_.end())
        {
            removeFromMain(it->second);
            mainCache_.erase(it);
        }
    }
    
    // 检查主缓存中是否有键为key的节点
    bool existsInMain(Key key) 
    {
        return mainCache_.find(key) != mainCache_.end();
    }



private:
    // 初始化主缓存双向链表、幽灵缓存双向链表
    void initializeLists() 
    {
        mainHead_ = std::make_shared<NodeType>();
        mainTail_ = std::make_shared<NodeType>();
        mainHead_->next_ = mainTail_;
        mainTail_->prev_ = mainHead_;

        ghostHead_ = std::make_shared<NodeType>();
        ghostTail_ = std::make_shared<NodeType>();
        ghostHead_->next_ = ghostTail_;
        ghostTail_->prev_ = ghostHead_;
    }

    // 将新节点添加至链表头部
    void addToFront(NodePtr node) 
    {
        node->next_ = mainHead_->next_;
        node->prev_ = mainHead_;
        mainHead_->next_->prev_ = node;
        mainHead_->next_ = node;
    }

    // 将节点移动到链表头部（最近访问位置）
    void moveToFront(NodePtr node) 
    {
        // 先把该节点从当前位置移除
        node->prev_->next_ = node->next_;
        node->next_->prev_ = node->prev_;
        
        // 添加到头部
        addToFront(node);
    }

    // 更新现有节点（值更新+移动至链表头）(在put方法中使用)
    bool updateExistingNode(NodePtr node, const Value& value) 
    {
        node->setValue(value);
        moveToFront(node);
        return true;
    }

    // 更新节点访问状态（移动至链表头+增加计数）(在get方法中使用)
    bool updateNodeAccess(NodePtr node) 
    {
        moveToFront(node);
        node->incrementAccessCount();
        return node->getAccessCount() >= transformThreshold_;   // 返回是否达到转移LFU的阈值
    }

    // 把节点从主缓存链表中移除 (仅调整指针)
    void removeFromMain(NodePtr node) 
    {
        node->prev_->next_ = node->next_;
        node->next_->prev_ = node->prev_;
    }

    // 把节点从幽灵缓存链表中移除
    void removeFromGhost(NodePtr node) 
    {
        node->prev_->next_ = node->next_;
        node->next_->prev_ = node->prev_;
    }

    // 添加节点到幽灵缓存
    void addToGhost(NodePtr node) 
    {
        // 重置节点的访问计数
        node->accessCount_ = 1;
        
        // 添加到幽灵缓存的头部
        node->next_ = ghostHead_->next_;
        node->prev_ = ghostHead_;
        ghostHead_->next_->prev_ = node;
        ghostHead_->next_ = node;
        
        // 添加到幽灵缓存映射 (幽灵缓存hash表)
        ghostCache_[node->getKey()] = node;
    }

    // 删除最旧的幽灵缓存节点
    void removeOldestGhost() 
    {
        NodePtr oldestGhost = ghostTail_->prev_;
        if (oldestGhost == ghostHead_)  //如果幽灵缓存链表已经空了
            return;

        removeFromGhost(oldestGhost);   //从链表中移除
        ghostCache_.erase(oldestGhost->getKey());   //从hsah表中删除
    }

    // 驱逐主缓存中最久未使用的节点
    void evictLeastRecent() 
    {
        NodePtr leastRecent = mainTail_->prev_;
        if (leastRecent == mainHead_)   //如果主缓存链表已经空了，就不操作
            return;

        // 从主缓存链表中移除
        removeFromMain(leastRecent);

        // 添加到幽灵缓存 (如果幽灵缓存已经满了，就先删除幽灵缓存中最旧的元素)
        if (ghostCache_.size() >= ghostCapacity_) 
        {
            removeOldestGhost();
        }
        addToGhost(leastRecent);

        // 从主缓存映射中删除
        mainCache_.erase(leastRecent->getKey());
    }

    // 向主缓存中添加一个新节点
    bool addNewNode(const Key& key, const Value& value) 
    {
        //如果主缓存已经满了，就驱逐主缓存中最久未使用的节点
        if (mainCache_.size() >= capacity_) 
        {   
            evictLeastRecent(); // 驱逐最近最少访问
        }

        NodePtr newNode = std::make_shared<NodeType>(key, value);
        mainCache_[key] = newNode;  //添加到主缓存hash表
        addToFront(newNode);    //添加到主缓存链表
        return true;
    }



private:
    size_t capacity_;           // 主缓存容量
    size_t ghostCapacity_;      // 幽灵缓存容量
    size_t transformThreshold_; // 转换门槛值(转移到lfu的访问次数阈值)
    std::mutex mutex_;          // 互斥锁

    NodeMap mainCache_;         // 主缓存hash表     key -> ArcNode
    NodeMap ghostCache_;        // 幽灵缓存hash表

    // 主链表
    NodePtr mainHead_;          // 主缓存虚拟头节点
    NodePtr mainTail_;          // 主缓存虚拟尾节点
    // 淘汰链表(幽灵链表)
    NodePtr ghostHead_;         // 幽灵缓存虚拟头节点
    NodePtr ghostTail_;         // 幽灵缓存虚拟尾节点

};

} // namespace MyCache