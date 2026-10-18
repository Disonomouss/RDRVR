#include "core/holster_record.h"
#include "holster_pose.h"
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

using V = std::array<double, 3>;
using M = std::array<float, 16>;
static int checks = 0;
static void check(bool ok, const char* why) {
    ++checks;
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", why); std::exit(1); }
}
static V point(const float* m, V p) {
    V out{};
    for (int r=0;r<3;++r) out[r]=m[r]*p[0]+m[4+r]*p[1]+m[8+r]*p[2]+m[12+r];
    return out;
}
static V delta(const float* a,const float* t,V p) {
    return {a[0]*p[0]+a[1]*p[1]+a[2]*p[2]+t[0],
            a[3]*p[0]+a[4]*p[1]+a[5]*p[2]+t[1],
            a[6]*p[0]+a[7]*p[1]+a[8]*p[2]+t[2]};
}
static void equal(V a,V b,const char* why,double tolerance=0.0015) {
    for(int r=0;r<3;++r) check(std::fabs(a[r]-b[r])<tolerance,why);
}
static M record(float yaw,float pitch,V p) {
    const float y=yaw*0.0174532925f,x=pitch*0.0174532925f;
    const float c=std::cos(y),s=std::sin(y),cp=std::cos(x),sp=std::sin(x);
    return {c,0,-s,0, s*sp,cp,c*sp,0, s*cp,-sp,c*cp,0,
            static_cast<float>(p[0]),static_cast<float>(p[1]),static_cast<float>(p[2]),1};
}
static void correction(const float* original,const float* target,float* a,float* t) {
    for(int r=0;r<3;++r) for(int c=0;c<3;++c) {
        a[r*3+c]=0;
        for(int k=0;k<3;++k) a[r*3+c]+=target[k*3+r]*original[k*4+c];
    }
    for(int r=0;r<3;++r) t[r]=target[9+r]-(a[r*3]*original[12]+a[r*3+1]*original[13]+a[r*3+2]*original[14]);
}
int main() {
    int cases=0;
    const V vertices[]={{0,0,0},{0,0,-0.4},{0,0.12,0.08},{-0.03,-0.06,0.04}};
    for(int heading=-180;heading<=180;heading+=15) {
        const float h=static_cast<float>(heading)*0.0174532925f;
        const float target[12]={std::cos(h),0,-std::sin(h),-std::sin(h),0,-std::cos(h),0,1,0,-839.9f,93.54f,2372.8f};
        for(int base=-180;base<180;base+=30) {
            const M current=record(static_cast<float>(base),17,{-839.8,93.5,2372.7});
            const M previous=record(static_cast<float>(base)-4,12,{-839.81,93.52,2372.73});
            std::array<float,34> storage{};storage.front()=12345;storage.back()=-23456;
            float* both=storage.data()+1;
            std::memcpy(both,current.data(),sizeof(current));std::memcpy(both+16,previous.data(),sizeof(previous));
            float a[9],t[3];correction(current.data(),target,a,t);
            rdrvr::holster_record::apply_delta(both,a,t);
            check(storage.front()==12345 && storage.back()==-23456,"record bounds");
            for(int half=0;half<2;++half) {
                const float* old=half?previous.data():current.data();
                const float* now=both+half*16;
                for(int i=3;i<16;i+=4)check(now[i]==old[i],"homogeneous components preserved");
                for(V v:vertices)equal(point(now,v),delta(a,t,point(old,v)),"record-only geometry follows world correction");
                // Six animated parts, including nonidentity rotations, offsets and a
                // scaled part: changing the outer record preserves their local pose.
                for(int bone=0;bone<6;++bone) {
                    M local=record(static_cast<float>(bone*31),static_cast<float>(bone*-7),{0.01*bone,0.02*bone,-0.03*bone});
                    if(bone==5)for(int i=0;i<12;++i)if(i%4!=3)local[i]*=0.8f;
                    const M saved=local;
                    for(V v:vertices)equal(point(now,point(local.data(),v)),delta(a,t,point(old,point(local.data(),v))),"all local parts retain corrected world pose");
                    check(local==saved,"local animation/bind data untouched");
                }
            }
            // Restoring the existing saved 32 floats restores the complete record.
            std::memcpy(both,current.data(),sizeof(current));std::memcpy(both+16,previous.data(),sizeof(previous));
            check(std::memcmp(both,current.data(),sizeof(current))==0 && std::memcmp(both+16,previous.data(),sizeof(previous))==0,"draw restoration");
            ++cases;
        }
    }
    // Regression: correcting only the local root can make root diagnostics pass
    // while a record-only consumer still sees an unturned world transform.
    const M identity=record(0,0,{0,0,0});
    const float wanted[12]={1,0,0,0,0,-1,0,1,0,0,0,0};
    const float turned_local[12]={1,0,0,0,0,0,1,0,0,-1,0,0};
    const auto misleading=rdrvr::holster_draw_diag::world_root(identity.data(),turned_local,nullptr,true);
    rdrvr::holster_draw_diag::Pose goal{};std::memcpy(goal.data(),wanted,sizeof(wanted));
    check(rdrvr::holster_draw_diag::max_axis_error_deg(misleading,goal)<0.05f,"old root diagnostic can pass");
    const auto outer=rdrvr::holster_draw_diag::world_root(identity.data(),nullptr,nullptr,true);
    check(rdrvr::holster_draw_diag::max_axis_error_deg(outer,goal)>89,"old outer record can remain wrong");
    std::printf("PASS: %d holster record checks (%d synthetic pose cases)\n", checks, cases);
    std::puts("Synthetic record/bone consumers only; headset acceptance required.");
}
