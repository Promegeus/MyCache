#pragma once

#include "../KICachePolicy.h"
#include "KArcLruPart.h"
#include "KArcLfuPart.h"
#include <memory>

namespace MyCache 
{

template<typename Key, typename Value>
class KArcCache : public KICachePolicy<Key, Value> 
{
public:
    explicit KArcCache(size_t capacity = 10, size_t transformThreshold = 2)
        : capacity_(capacity)
        , transformThreshold_(transformThreshold)
        , lruPart_(std::make_unique<ArcLruPart<Key, Value>>(capacity/2, transformThreshold))
        , lfuPart_(std::make_unique<ArcLfuPart<Key, Value>>(capacity/2, transformThreshold))
    {}

    ~KArcCache() override = default;

    void put(Key key, Value value) override 
    {
        // 若幽灵缓存中有该节点，则删除，并调整主缓存大小
        Value ghostValue;
        bool inGhost = checkGhostCaches(key, ghostValue);
        
        bool inLru = lruPart_->existsInMain(key);
        bool inLfu = lfuPart_->existsInMain(key);

         //如果在主缓存中，则更新
        if (inLru || inLfu)    
        {
            // 如果在lfu主缓存中，则更新LFU中的现有项
            if (inLfu)
                lfuPart_->put(key, value); 

            // 如果在lru主缓存中，则更新LRU中的现有项，并检查是否达到迁移阈值，若达到则迁移至LFU
            if (inLru) 
            {
                bool shouldTransform = lruPart_->put(key, value);
                if (shouldTransform) 
                {
                    lfuPart_->put(key, value);
                    lruPart_->remove(key);  // 从LRU移除
                }

            }
            return;
        }

        //如果不在幽灵缓存，添加至lru中即可
        lruPart_->put(key, value);
    }

    bool get(Key key, Value& value) override 
    {
        Value ghostValue;
        bool inGhost = checkGhostCaches(key, ghostValue);

        bool inLru = lruPart_->existsInMain(key);
        bool inLfu = lfuPart_->existsInMain(key);

        // 只要在主缓存中，说明命中了
        if(inLru || inLfu)
        {
            if(inLfu)
                lfuPart_->get(key, value);
            if(inLru)
            {
                bool shouldTransform = false;
                lruPart_->get(key, value, shouldTransform);
                if (shouldTransform) 
                {
                    lfuPart_->put(key, value);
                    lruPart_->remove(key);  //保证一个节点只存在于一个主缓存中
                }
            }
            return true;
        }

        //如果不在主缓存，但在幽灵缓存，说明刚被淘汰，应加入lru主缓存中
        if(inGhost)
        {
            lruPart_->put(key, ghostValue);
        }
        return false;

    }

    Value get(Key key) override 
    {
        Value value{};
        get(key, value);
        return value;
    }

private:
    bool checkGhostCaches(Key key, Value& gValue) 
    {
        bool inGhost = false;
        if (lruPart_->checkGhost(key, gValue)) 
        {
            if (lfuPart_->decreaseCapacity()) 
            {
                lruPart_->increaseCapacity();
            }
            inGhost = true;
        } 
        if (lfuPart_->checkGhost(key, gValue)) 
        {
            if (lruPart_->decreaseCapacity()) 
            {
                lfuPart_->increaseCapacity();
            }
            inGhost = true;
        }
        return inGhost;
    }

private:
    size_t capacity_;   //总容量
    size_t transformThreshold_;     //lru中的数据转lfu的访问次数阈值
    std::unique_ptr<ArcLruPart<Key, Value>> lruPart_;   //指向lru组件的指针
    std::unique_ptr<ArcLfuPart<Key, Value>> lfuPart_;   //指向lfu组件的指针
};

} // namespace MyCache