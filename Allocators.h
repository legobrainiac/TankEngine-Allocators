//////////////////////////////////////////////////////////////////////////
// Author: Tomas Antonio Sanches Pinto, TankEngine v4, 2020
// File: Pool.h
//////////////////////////////////////////////////////////////////////////

#ifndef POOL_H
#define POOL_H

#include <type_traits>
#include <functional>
#include <mutex>

#include <cstdlib>
#include <memory>
#include <tuple>
#include <array>
#include <map>
#include <new>

template <uint64_t L, uint64_t R>
struct constexpr_mod
{
	static constexpr uint64_t value = L % R;
};

namespace Alc
{
    class Allocator {};

    struct PoolAllocator
    {
    public:
        [[nodiscard]] constexpr virtual auto Internal() noexcept -> void* = 0;
    };

    ////////////////////////////////////////////////
    // OffsetPtr
    template<typename Type>
    class OffsetPtr
    {
    public:
        OffsetPtr()
            : pContainer(nullptr)
            , offset(0U)
        {
        }

        OffsetPtr(PoolAllocator* pPool, uint64_t elementOffset)
            : pContainer(pPool)
            , offset(elementOffset)
        {
        }

        // Helpers
        [[nodiscard]] constexpr auto Internal() noexcept -> uint64_t { return offset; }
        [[nodiscard]] constexpr auto Container() noexcept -> PoolAllocator* { return pContainer; }

        constexpr void ZeroOut() noexcept
        {
            offset = 0U;
            pContainer = nullptr;
        }

        // Resolving
        [[nodiscard]] constexpr auto Resolve() noexcept -> Type* { return static_cast<Type*>(pContainer->Internal()) + offset; }
        constexpr Type* operator->() noexcept { return static_cast<Type*>(pContainer->Internal()) + offset; }

    private:
        uint64_t offset;
        PoolAllocator *pContainer;
    };

    template<typename Type>
    using OPtr = OffsetPtr<Type>;

    ////////////////////////////////////////////////
    // AlignedAllocator
    class AlignedAllocator
        : public Allocator
    {
    public:
        [[nodiscard]] static auto Alloc(size_t size, uint64_t alnm) -> void*
        {
            void *pMem = _aligned_malloc(size, alnm);
            std::memset(pMem, 0, size);
            return pMem;
        }
        
        static void Dealloc(void *pPtr)
        {
            _aligned_free(pPtr);
        }
    };
    
    struct Pool
        : public PoolAllocator, public Allocator
    {
        union
        {
            void *pLut;
            void *pPoolBlockStart;
        };

        void *pMem              = nullptr;
        uint64_t capacity       = 0U;
        uint64_t size           = 0U;
        uint64_t poolItemSize   = 0U;

        Pool()
        {
            capacity        = 0U;
            size            = 0U;
            poolItemSize    = 0U;
            pLut            = nullptr;
            pMem            = nullptr;
        }

        explicit Pool(uint64_t elementSize, uint64_t poolCapacity = 1024U)
        {
            void *pMemory = AlignedAllocator::Alloc((poolCapacity / 8U) + (elementSize * poolCapacity), elementSize);

            capacity        = poolCapacity;
            size            = 0U;
            poolItemSize    = elementSize;
            pLut            = pMemory;
            pMem            = static_cast<uint8_t*>(pMemory) + (poolCapacity / 8U);
        }

        void Reallocate()
        {
            Pool old(*this);

            // Duplicate capacity
            capacity *= 2;

            // Reallocate and assign accordingly
            void *pNewMemory = AlignedAllocator::Alloc((capacity / 8U) + (poolItemSize * capacity), poolItemSize);

            pLut = pNewMemory;
            pMem = static_cast<uint8_t*>(pNewMemory) + (capacity / 8U);

            // Copy over old LUT
            std::memcpy(pPoolBlockStart, old.pPoolBlockStart, old.capacity / 8U);

            // Copy over old pool
            std::memcpy(pMem, old.pMem, old.poolItemSize * old.capacity);

            // Old goes out of scope and is freed
        }

        [[nodiscard]] constexpr auto Internal() noexcept -> void* override { return pMem; };

        ~Pool()
        {
            AlignedAllocator::Dealloc(static_cast<uint8_t*>(pPoolBlockStart));
        }

    private:
        Pool(const Pool& other)
        {
            pLut            = other.pLut;
            pPoolBlockStart = other.pPoolBlockStart;
            pMem            = other.pMem;
            capacity        = other.capacity;
            size            = other.size;
            poolItemSize    = other.poolItemSize;
        }
    };

    ////////////////////////////////////////////////
    // FixedSizeAllocator
    class Fsa
        : public Allocator
    {
    public:
        template<uint64_t Size, uint64_t Capacity = 1024U>
        [[nodiscard]] static auto GetPool() -> Pool*
        {
            static Pool p(Size, Capacity);
            return &p;
        }
    };

    ////////////////////////////////////////////////
    // Callbacks
    using OnReallocateCallback = std::function<void()>;

    ////////////////////////////////////////////////
    // FixedTypeAllocator
    template <
            typename Type,
            uint64_t Size       = 1024U,
            bool Reallocates    = true,
            bool ThreadSafe     = false,
            std::enable_if_t<constexpr_mod<Size, 8>::value == 0, int> = 0>
    class FixedTypeAllocator
        : public Allocator
    {
    public:
        FixedTypeAllocator()
            : m_Pool(sizeof(Type), Size)
        {
        }

        void SetOnReallocateCallback(const OnReallocateCallback& cb) { m_OnReallocateCallback = cb; }

        [[nodiscard]] constexpr auto Internal() noexcept -> Pool * { return &m_Pool; }
        [[nodiscard]] constexpr auto Get() -> OffsetPtr<Type>
        {
            // In case we're in a thread safe pool, lock
            if constexpr(ThreadSafe)
                m_Mutex.lock();

            if (m_Pool.size + 1 > m_Pool.capacity)
            {
                if constexpr (Reallocates)
                {
                    m_Pool.Reallocate();
                    m_OnReallocateCallback();
                }
                else
                    return OffsetPtr<Type>{};
            }

            // Look up first available slot in pool
            auto pLut = static_cast<uint8_t *>(m_Pool.pLut);

            for (uint64_t i = 0; i < m_Pool.capacity / 8; ++i)
            {
                // Check pool map for inactive entities
                for (uint64_t j = 0; j < 8; ++j)
                {
                    uint8_t flag = (1U << j);

                    // Check inverse byte against flag
                    if (~(*pLut) & flag)
                    {
                        *pLut |= flag;
                        m_Pool.size++;

                        // Once the lut and the pool size have been mutated,
                        //  In case we're in a thread safe pool, unlock
                        if constexpr(ThreadSafe)
                            m_Mutex.unlock();

                        // Placement if to get an initialized object
                        Type *pFreeObject = &static_cast<Type*>(m_Pool.pMem)[i * 8 + j];
                        new (pFreeObject)Type();

                        OffsetPtr<Type> offsetPtr (&m_Pool, i * 8U + j);

                        return offsetPtr;
                    }
                }

                pLut++;
            }

            // At this point something failed in the allocation
            throw std::bad_alloc();
        }

        constexpr void Pop(OffsetPtr<Type>& element)
        {
            // In case we're in a thread safe pool, lock
            if constexpr(ThreadSafe)
                m_Mutex.lock();

            Type* pElement = element.Resolve();

            if (pElement < m_Pool.pMem || pElement > static_cast<Type*>(m_Pool.pMem) + m_Pool.capacity)
                throw std::out_of_range("Address of pElement out of bounds of memory pool...");

            auto pLut       = static_cast<uint8_t*>(m_Pool.pLut);
            auto pMemory    = static_cast<Type*>(m_Pool.pMem);
            auto index      = static_cast<uint64_t>(pElement - pMemory);

            // Get box in look up table
            uint8_t *pBox = &pLut[0];
            uint8_t boxOffset = 0U;

            if (index != 0U)
            {
                pBox = &pLut[(index / 8U) % m_Pool.capacity];
                boxOffset = index % 8U;
            }

            // If item active, disable it
            uint8_t flag = (1U << boxOffset);
            if (*pBox & flag)
            {
                // Flip bit
                *pBox ^= flag;

                // Decrease used size
                m_Pool.size--;

                // Once the lut and the pool size have been mutated,
                //  in case we're in a thread safe pool, unlock
                if constexpr(ThreadSafe)
                    m_Mutex.unlock();

                // Since we're not calling delete, ~Type() is called manually
                pMemory[index].~Type();
                element.ZeroOut();
            }
        }

    private:
        void ForAllActive(const std::function<void(Type*)>& f)
        {
            if constexpr(ThreadSafe)
                m_Mutex.lock();

            auto pPoolItem  = static_cast<Type*>(m_Pool.pMem);
            auto pLut       = static_cast<uint8_t*>(m_Pool.pLut);

            for (uint64_t i = 0; i < m_Pool.capacity; ++i)
            {
                // Get box in look up table
                uint8_t *pBox = &pLut[0];
                uint8_t boxOffset = 0U;

                if (i != 0U)
                {
                    pBox = &pLut[(i / 8) % Size];
                    boxOffset = i % 8;
                }

                // If item active
                uint8_t flag = (1U << boxOffset);
                if (*pBox & flag)
                    f(pPoolItem);

                pPoolItem++;
            }

            if constexpr(ThreadSafe)
                m_Mutex.unlock();
        }

        void ForAllFast(const std::function<void(Type*)>& f)
        {
            if constexpr(ThreadSafe)
                m_Mutex.lock();

            auto pPoolItem = static_cast<Type*>(m_Pool.pMem);

            for (uint64_t i = 0; i < m_Pool.capacity; ++i)
            {
                f(pPoolItem);
                ++pPoolItem;
            }

            if constexpr(ThreadSafe)
                m_Mutex.unlock();
        }

    public:
        template <bool IgnoreInactive = true>
        void ForAll(const std::function<void(Type*)>& f)
        {
            if constexpr(IgnoreInactive)
                ForAllActive(f);
            else
                ForAllFast(f);
        }

    private:
        Pool m_Pool;
        OnReallocateCallback m_OnReallocateCallback = [](){};
        std::mutex m_Mutex;
    };

    ////////////////////////////////////////////////
    // GeneralPurposeAllocator
    template<uint64_t Size>
    struct Padding
    {
    private:
        uint8_t internal[Size];
    };

    template<uint64_t PoolSize  = 128U,
            bool Reallocates    = true,
            bool ThreadSafe     = false,
            uint64_t ...SubPoolSize>
    class GeneralPurposeAllocator
    {
    private:
        template<uint64_t Size>
        [[nodiscard]] auto AddPools() -> uint64_t
        {
            m_Pools[Size] = new FixedTypeAllocator<Padding<Size>, PoolSize, Reallocates, ThreadSafe>();
            return Size;
        }

    public:
        GeneralPurposeAllocator()
        {
            constexpr auto subPoolCount = sizeof...(SubPoolSize);
            if constexpr (subPoolCount > 0)
            {
                // Unwrapping with std::array
                 m_SubPoolSizes = { AddPools<SubPoolSize>()... };
            }
        }

    private:
        template<typename Type>
        [[nodiscard]] constexpr auto ResolvePool() -> uint64_t
        {
            constexpr auto subPoolCount = sizeof...(SubPoolSize);
            constexpr auto typeSize     = sizeof(Type);

            for (uint64_t i = 0; i < subPoolCount; ++i)
            {
                if (typeSize <= m_SubPoolSizes[i])
                    return m_SubPoolSizes[i];
            }

            throw std::bad_alloc();
        }

    public:
        template<typename Type>
        [[nodiscard]] constexpr auto New() -> OffsetPtr<Type>
        {
            using PoolType = FixedTypeAllocator<Padding<sizeof(Type)>, PoolSize, Reallocates, ThreadSafe>;

            uint64_t pool = ResolvePool<Type>();
            auto ftPool = static_cast<PoolType*>(m_Pools[pool]);

            return OffsetPtr<Type> ((PoolAllocator*)ftPool, ftPool->Get().Internal());
        }

        template<typename Type>
        constexpr void Delete(OffsetPtr<Type>& oPtr)
        {
            using SizedType = Padding<sizeof(Type)>;
            using PoolType  = FixedTypeAllocator<Padding<sizeof(Type)>, PoolSize, Reallocates, ThreadSafe>;

            uint64_t pool = ResolvePool<Type>();
            auto ftPool = static_cast<PoolType*>(m_Pools[pool]);

            // TODO(tomas): not ideal
            OffsetPtr<SizedType> sized { (PoolAllocator*)ftPool, oPtr.Internal() };
            ftPool->Pop(sized);
            oPtr.ZeroOut();
        }

        ~GeneralPurposeAllocator()
        {
            for (auto pool : m_Pools)
                delete pool.second;
        }

    private:
        std::map<uint64_t, Allocator*> m_Pools{};
        std::array<uint64_t, sizeof...(SubPoolSize)> m_SubPoolSizes{};
        uint64_t m_PoolInitializationCounter = 0U;
    };
}

#endif // !POOL_H
