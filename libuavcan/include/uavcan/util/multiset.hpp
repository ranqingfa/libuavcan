/*
 * Copyright (C) 2014 Pavel Kirienko <pavel.kirienko@gmail.com>
 */

#ifndef UAVCAN_UTIL_MULTISET_HPP_INCLUDED
#define UAVCAN_UTIL_MULTISET_HPP_INCLUDED

#include <cassert>
#include <cstdlib>
#include <uavcan/util/linked_list.hpp>
#include <uavcan/build_config.hpp>
#include <uavcan/dynamic_memory.hpp>
#include <uavcan/util/templates.hpp>
#include <uavcan/util/placement_new.hpp>

#if !defined(UAVCAN_CPP_VERSION) || !defined(UAVCAN_CPP11)
# error UAVCAN_CPP_VERSION
#endif

namespace uavcan
{
/**
 * Slow but memory efficient unordered multiset. Unlike Map<>, this container does not move objects, so
 * they don't have to be copyable.
 *
 * Items can be allocated in a static buffer or in the node's memory pool if the static buffer is exhausted.
 *
 * Number of static entries must not be less than 1.
 */
template <typename T, unsigned NumStaticEntries>
class UAVCAN_EXPORT Multiset : Noncopyable
{
    struct Item : ::uavcan::Noncopyable
    {
        T* ptr;

#if UAVCAN_CPP_VERSION >= UAVCAN_CPP11
        alignas(T) unsigned char pool[sizeof(T)];       ///< Memory efficient version
#else
        union
        {
            unsigned char pool[sizeof(T)];
            /*
             * Such alignment does not guarantee safety for all types (only for libuavcan internal ones);
             * however, increasing it is too memory inefficient. So it is recommended to use C++11, where
             * this issue is resolved with alignas() (see above).
             */
            void* _aligner1_;
            long long _aligner2_;
        };
#endif

        Item()
            : ptr(NULL)
        {
            fill_n(pool, sizeof(pool), static_cast<unsigned char>(0));
        }

        ~Item() { destroy(); }

        bool isConstructed() const { return ptr != NULL; }

        void destroy()
        {
            if (ptr != NULL)
            {
                ptr->~T();
                ptr = NULL;
                fill_n(pool, sizeof(pool), static_cast<unsigned char>(0));
            }
        }
    };

private:
    struct Chunk : LinkedListNode<Chunk>, ::uavcan::Noncopyable
    {
        enum { NumItems = (MemPoolBlockSize - sizeof(LinkedListNode<Chunk>)) / sizeof(Item) };
        Item items[NumItems];

        Chunk()
        {
            StaticAssert<(static_cast<unsigned>(NumItems) > 0)>::check();
            IsDynamicallyAllocatable<Chunk>::check();
            UAVCAN_ASSERT(!items[0].isConstructed());
        }

        static Chunk* instantiate(IPoolAllocator& allocator)
        {
            void* const praw = allocator.allocate(sizeof(Chunk));
            if (praw == NULL)
            {
                return NULL;
            }
            return new (praw) Chunk();
        }

        static void destroy(Chunk*& obj, IPoolAllocator& allocator)
        {
            if (obj != NULL)
            {
                obj->~Chunk();
                allocator.deallocate(obj);
                obj = NULL;
            }
        }

        Item* findFreeSlot()
        {
            for (unsigned i = 0; i < static_cast<unsigned>(NumItems); i++)
            {
                if (!items[i].isConstructed())
                {
                    return items + i;
                }
            }
            return NULL;
        }
    };

    /*
     * Data
     */
    LinkedListRoot<Chunk> list_;
    IPoolAllocator& allocator_;
    Item static_[NumStaticEntries];

    /*
     * Methods
     */
    Item* findOrCreateFreeSlot();

    void compact();

    enum RemoveStrategy { RemoveOne, RemoveAll };

    template <typename Predicate>
    void removeWhere(Predicate predicate, RemoveStrategy strategy);

    struct YesPredicate
    {
        bool operator()(const T&) const { return true; }
    };

    struct IndexPredicate : ::uavcan::Noncopyable
    {
        unsigned index;
        IndexPredicate(unsigned target_index)
            : index(target_index)
        { }

        bool operator()(const T&)
        {
            return index-- == 0;
        }
    };

    struct ComparingPredicate
    {
        const T& reference;

        ComparingPredicate(const T& ref)
            : reference(ref)
        { }

        bool operator()(const T& sample)
        {
            return reference == sample;
        }
    };

    template<typename Operator>
    struct OperatorToFalsePredicateAdapter : ::uavcan::Noncopyable
    {
        Operator oper;

        OperatorToFalsePredicateAdapter(Operator o)
            : oper(o)
        { }

        bool operator()(T& item)
        {
            oper(item);
            return false;
        }

        bool operator()(const T& item) const
        {
            oper(item);
            return false;
        }
    };

public:
    Multiset(IPoolAllocator& allocator)
        : allocator_(allocator)
    { }

    ~Multiset()
    {
        clear();
    }

    /**
     * Creates one item in-place and returns a pointer to it.
     * If creation fails due to lack of memory, NULL will be returned.
     * Complexity is O(N).
     */
    T* emplace()
    {
        Item* const item = findOrCreateFreeSlot();
        if (item == NULL)
        {
            return NULL;
        }
        UAVCAN_ASSERT(item->ptr == NULL);
        item->ptr = new (item->pool) T();
        return item->ptr;
    }

    template <typename P1>
    T* emplace(P1 p1)
    {
        Item* const item = findOrCreateFreeSlot();
        if (item == NULL)
        {
            return NULL;
        }
        UAVCAN_ASSERT(item->ptr == NULL);
        item->ptr = new (item->pool) T(p1);
        return item->ptr;
    }

    template <typename P1, typename P2>
    T* emplace(P1 p1, P2 p2)
    {
        Item* const item = findOrCreateFreeSlot();
        if (item == NULL)
        {
            return NULL;
        }
        UAVCAN_ASSERT(item->ptr == NULL);
        item->ptr = new (item->pool) T(p1, p2);
        return item->ptr;
    }

    template <typename P1, typename P2, typename P3>
    T* emplace(P1 p1, P2 p2, P3 p3)
    {
        Item* const item = findOrCreateFreeSlot();
        if (item == NULL)
        {
            return NULL;
        }
        UAVCAN_ASSERT(item->ptr == NULL);
        item->ptr = new (item->pool) T(p1, p2, p3);
        return item->ptr;
    }

    /**
     * Removes entries where the predicate returns true.
     * Predicate prototype:
     *  bool (T& item)
     */
    template <typename Predicate>
    void removeAllWhere(Predicate predicate) { removeWhere<Predicate>(predicate, RemoveAll); }

    template <typename Predicate>
    void removeFirstWhere(Predicate predicate) { removeWhere<Predicate>(predicate, RemoveOne); }

    void removeFirst(const T& ref) { removeFirstWhere(ComparingPredicate(ref)); }

    void removeAll(const T& ref) { removeAllWhere(ComparingPredicate(ref)); }

    void clear() { removeAllWhere(YesPredicate()); }

    /**
     * Returns first entry where the predicate returns true.
     * Predicate prototype:
     *  bool (const T& item)
     */
    template <typename Predicate>
    T* find(Predicate predicate);

    template <typename Predicate>
    const T* find(Predicate predicate) const
    {
        return const_cast<Multiset*>(this)->find<Predicate>(predicate);
    }

    /**
     * Calls Operator for each item of the set.
     * Operator prototype:
     *  void (T& item)
     *  void (const T& item) - const overload
     */
    template <typename Operator>
    void forEach(Operator oper)
    {
        OperatorToFalsePredicateAdapter<Operator> adapter(oper);
        (void)find<OperatorToFalsePredicateAdapter<Operator>&>(adapter);
    }

    template <typename Operator>
    void forEach(Operator oper) const
    {
        const OperatorToFalsePredicateAdapter<Operator> adapter(oper);
        (void)find<const OperatorToFalsePredicateAdapter<Operator>&>(adapter);
    }

    /**
     * Returns an item located at the specified position from the beginning.
     * Note that addition and removal operations invalidate indices.
     * If index is greater than or equal the number of items, null pointer will be returned.
     * Complexity is O(N).
     */
    T* getByIndex(unsigned index)
    {
        IndexPredicate predicate(index);
        return find<IndexPredicate&>(predicate);
    }

    const T* getByIndex(unsigned index) const
    {
        return const_cast<Multiset*>(this)->getByIndex(index);
    }

    /**
     * Complexity is O(1).
     */
    bool isEmpty() const { return find(YesPredicate()) == NULL; }

    /**
     * Counts number of items stored.
     * Best case complexity is O(N).
     */
    unsigned getSize() const { return getNumStaticItems() + getNumDynamicItems(); }

    /**
     * For testing, do not use directly.
     */
    unsigned getNumStaticItems() const;
    unsigned getNumDynamicItems() const;
};

// ----------------------------------------------------------------------------

/*
 * Multiset<>
 */
template <typename T, unsigned NumStaticEntries>
typename Multiset<T, NumStaticEntries>::Item* Multiset<T, NumStaticEntries>::findOrCreateFreeSlot()
{
#if !UAVCAN_TINY
    // Search in static pool
    for (unsigned i = 0; i < NumStaticEntries; i++)
    {
        if (!static_[i].isConstructed())
        {
            return &static_[i];
        }
    }
#endif

    // Search in dynamic pool
    {
        Chunk* p = list_.get();
        while (p)
        {
            Item* const dyn = p->findFreeSlot();
            if (dyn != NULL)
            {
                return dyn;
            }
            p = p->getNextListNode();
        }
    }

    // Create new dynamic chunk
    Chunk* const chunk = Chunk::instantiate(allocator_);
    if (chunk == NULL)
    {
        return NULL;
    }
    list_.insert(chunk);
    return &chunk->items[0];
}

template <typename T, unsigned NumStaticEntries>
void Multiset<T, NumStaticEntries>::compact()
{
    Chunk* p = list_.get();
    while (p)
    {
        Chunk* const next = p->getNextListNode();
        bool remove_this = true;
        for (int i = 0; i < Chunk::NumItems; i++)
        {
            if (p->items[i].isConstructed())
            {
                remove_this = false;
                break;
            }
        }
        if (remove_this)
        {
            list_.remove(p);
            Chunk::destroy(p, allocator_);
        }
        p = next;
    }
}

template <typename T, unsigned NumStaticEntries>
template <typename Predicate>
void Multiset<T, NumStaticEntries>::removeWhere(Predicate predicate, const RemoveStrategy strategy)
{
    unsigned num_removed = 0;

#if !UAVCAN_TINY
    for (unsigned i = 0; i < NumStaticEntries; i++)
    {
        if (static_[i].isConstructed())
        {
            if (predicate(*static_[i].ptr))
            {
                num_removed++;
                static_[i].destroy();
                if (strategy == RemoveOne)
                {
                    break;
                }
            }
        }
    }
#endif

    Chunk* p = list_.get();
    while (p != NULL)
    {
        Chunk* const next_chunk = p->getNextListNode(); // For the case if the current entry gets modified

        if ((num_removed > 0) && (strategy == RemoveOne))
        {
            break;
        }

        for (int i = 0; i < Chunk::NumItems; i++)
        {
            Item& item = p->items[i];
            if (item.isConstructed())
            {
                if (predicate(*item.ptr))
                {
                    num_removed++;
                    item.destroy();
                    if (strategy == RemoveOne)
                    {
                        break;
                    }
                }
            }
        }

        p = next_chunk;
    }

    if (num_removed > 0)
    {
        compact();
    }
}

template <typename T, unsigned NumStaticEntries>
template <typename Predicate>
T* Multiset<T, NumStaticEntries>::find(Predicate predicate)
{
#if !UAVCAN_TINY
    for (unsigned i = 0; i < NumStaticEntries; i++)
    {
        if (static_[i].isConstructed())
        {
            if (predicate(*static_[i].ptr))
            {
                return static_[i].ptr;
            }
        }
    }
#endif

    Chunk* p = list_.get();
    while (p != NULL)
    {
        Chunk* const next_chunk = p->getNextListNode(); // For the case if the current entry gets modified

        for (int i = 0; i < Chunk::NumItems; i++)
        {
            if (p->items[i].isConstructed())
            {
                if (predicate(*p->items[i].ptr))
                {
                    return p->items[i].ptr;
                }
            }
        }

        p = next_chunk;
    }
    return NULL;
}

template <typename T, unsigned NumStaticEntries>
unsigned Multiset<T, NumStaticEntries>::getNumStaticItems() const
{
    unsigned num = 0;
#if !UAVCAN_TINY
    for (unsigned i = 0; i < NumStaticEntries; i++)
    {
        num += static_[i].isConstructed() ? 1U : 0U;
    }
#endif
    return num;
}

template <typename T, unsigned NumStaticEntries>
unsigned Multiset<T, NumStaticEntries>::getNumDynamicItems() const
{
    unsigned num = 0;
    Chunk* p = list_.get();
    while (p)
    {
        for (int i = 0; i < Chunk::NumItems; i++)
        {
            num += p->items[i].isConstructed() ? 1U : 0U;
        }
        p = p->getNextListNode();
    }
    return num;
}

}

#endif // Include guard
