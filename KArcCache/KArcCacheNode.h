#pragma once

#include <memory>

namespace MyCache 
{

template<typename Key, typename Value>
class ArcNode 
{
private:
    Key key_;
    Value value_;
    size_t accessCount_;    //访问计数器(用于lfu逻辑)
    std::shared_ptr<ArcNode> prev_;     //双向链表前驱指针
    std::shared_ptr<ArcNode> next_;     //双向链表后继指针
    

public:
    ArcNode() : accessCount_(1), prev_(nullptr), next_(nullptr) {}
    // 构造函数：初始化键值对，访问次数默认为1
    ArcNode(Key key, Value value) 
        : key_(key)
        , value_(value)
        , accessCount_(1)
        , prev_(nullptr)
        , next_(nullptr) 
    {}

    // Getters
    Key getKey() const { return key_; }
    Value getValue() const { return value_; }
    size_t getAccessCount() const { return accessCount_; }
    
    // Setters
    void setValue(const Value& value) { value_ = value; }
    void incrementAccessCount() { ++accessCount_; }     //访问次数+1

    // 友元声明：允许LRU部分和LFU部分访问私有成员
    template<typename K, typename V> friend class ArcLruPart;
    template<typename K, typename V> friend class ArcLfuPart;
};

} // namespace MyCache