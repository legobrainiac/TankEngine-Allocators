
#include <chrono>

#include "Allocators.h"

template <typename TimeType>
class Bench
{
public:
    Bench()
    {
        m_Start = std::chrono::high_resolution_clock::now();
    }

    ~Bench()
    {
        auto end = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration<float>(end - m_Start);
        std::cout << std::chrono::duration_cast<TimeType>(duration).count() << std::endl;
    }

private:
    std::chrono::time_point<std::chrono::high_resolution_clock> m_Start{};
};

void TestFixedTypeAllocator()
{
    class Particle
    {
    public:
        void Update(float dt)
        {
            y -= 9.81f * dt;
            x += dt;
        }

        float x, y;
    };

    const auto dt = 1 / 60.f;
    Alc::FixedTypeAllocator<Particle, 128 K, true> allocator{};

    for (int i = 0; i < 128 K; ++i)
        allocator.Get()->x = (float)i;

    std::cout << "Capacity: " << allocator.Internal()->capacity << std::endl;

    // Smart
    std::cout << "Smart: ";
    {
        Bench<std::chrono::milliseconds> b{};

        for (int i = 0; i < 60; ++i)
        {
            allocator.ForAll([dt](Particle* pParticle) {
                pParticle->Update(dt);
            });
        }
    }

    // Fast
    std::cout << "Fast:";
    {
        Bench<std::chrono::milliseconds> b{};

        for (int i = 0; i < 60; ++i)
        {
            allocator.ForAll<false>([dt](Particle *pParticle) {
                pParticle->Update(dt);
            });
        }
    }
}

void TestGeneralPurposeAllocator()
{
    Alc::GeneralPurposeAllocator<128U, true, false, 8U, 16U, 32U, 64U, 128U, 256U> gpa{};

    struct WeirdStruct
    {
    public:
        uint8_t internal[48];
    };

    for (int i = 0; i < 10; ++i)
    {
        auto n = gpa.New<WeirdStruct>();
        std::cout << "Internal => " << n.Internal() << " Resolution => " << n.Resolve() << " Container => " << n.Container() << std::endl;
    }
}

void main()
{
    TestFixedTypeAllocator();
    TestGeneralPurposeAllocator();
}