// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: LGPL-2.1-or-later
// Test control logic without a GPU or D3DMetal. Include the implementation to
// inject a stand-in for the private heap switch and hook synthetic classes.
#import "../../src/in_process/graphics_interop_native.m"
static int subclass_calls, base_calls;
static id passed_descriptor;
static NSUInteger passed_offset;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); return 1; } } while (0)
@interface ControlBase : NSObject
-(id)newEvent;
-(id)newTextureWithDescriptor:(id)descriptor;
-(id)newTextureWithDescriptor:(id)descriptor offset:(NSUInteger)offset;
@end
@implementation ControlBase
-(id)newEvent { ++base_calls; return nil; }
-(id)newTextureWithDescriptor:(id)descriptor { ++base_calls; passed_descriptor = descriptor; return nil; }
-(id)newTextureWithDescriptor:(id)descriptor offset:(NSUInteger)offset
{ ++base_calls; passed_descriptor = descriptor; passed_offset = offset; return nil; }
@end
@interface ControlChild : ControlBase
@end
@implementation ControlChild
// Bound recursion so a regression fails cleanly instead of overflowing.
-(id)newEvent { if (++subclass_calls > 5) return nil; return [super newEvent]; }
-(id)newTextureWithDescriptor:(id)descriptor
{ if (++subclass_calls > 5) return nil; return [super newTextureWithDescriptor:descriptor]; }
-(id)newTextureWithDescriptor:(id)descriptor offset:(NSUInteger)offset
{ if (++subclass_calls > 5) return nil; return [super newTextureWithDescriptor:descriptor offset:offset]; }
@end
static void *overlap(void *unused)
{
    struct mw_gfx_native_params p = {0};
    arm_texture(&p); disarm_texture(&p);
    return NULL;
}
static pthread_mutex_t overlap_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t overlap_cond = PTHREAD_COND_INITIALIZER;
static int stage;
static void *held_overlap(void *unused)
{
    struct mw_gfx_native_params p = {0};
    arm_texture(&p);
    pthread_mutex_lock(&overlap_mutex);
    stage = 1; pthread_cond_signal(&overlap_cond);
    while (stage != 2) pthread_cond_wait(&overlap_cond, &overlap_mutex);
    pthread_mutex_unlock(&overlap_mutex);
    disarm_texture(&p);
    return NULL;
}
int main(void)
{
    @autoreleasepool {
        Class classes[] = {[ControlBase class], [ControlChild class]};
        for (unsigned i = 0; i < 2; ++i) {
            install(classes[i], @selector(newEvent), (IMP)hook_device_event);
            install(classes[i], @selector(newTextureWithDescriptor:), (IMP)hook_heap_texture);
            install(classes[i], @selector(newTextureWithDescriptor:offset:), (IMP)hook_heap_texture_offset);
        }
        ControlChild *child = [[ControlChild alloc] init];
        [child newEvent];
        CHECK(subclass_calls == 1 && base_calls == 1);
        subclass_calls = base_calls = 0;
        [child newTextureWithDescriptor:child];
        CHECK(subclass_calls == 1 && base_calls == 1 && passed_descriptor == child);
        subclass_calls = base_calls = 0;
        [child newTextureWithDescriptor:child offset:123];
        CHECK(subclass_calls == 1 && base_calls == 1 && passed_descriptor == child && passed_offset == 123);
        puts("PASS superclass forwarding and arguments for all hook signatures");
        unsigned char setting = 1;
        internal_heaps = &setting;
        struct mw_gfx_native_params p = {0};
        arm_texture(&p);
        CHECK(setting == 0 && heap_users == 1);
        pthread_t other;
        CHECK(pthread_create(&other, NULL, overlap, NULL) == 0);
        CHECK(pthread_join(other, NULL) == 0);
        CHECK(setting == 0 && heap_users == 1);
        disarm_texture(&p);
        CHECK(setting == 1 && heap_users == 0);
        // Also finish the original import while the other thread remains armed.
        arm_texture(&p);
        CHECK(pthread_create(&other, NULL, held_overlap, NULL) == 0);
        pthread_mutex_lock(&overlap_mutex);
        while (stage != 1) pthread_cond_wait(&overlap_cond, &overlap_mutex);
        disarm_texture(&p);
        CHECK(setting == 0 && heap_users == 1);
        stage = 2; pthread_cond_signal(&overlap_cond);
        pthread_mutex_unlock(&overlap_mutex);
        CHECK(pthread_join(other, NULL) == 0);
        CHECK(setting == 1 && heap_users == 0);
        // Preserve a switch that was already disabled, and tolerate a repeated disarm.
        setting = 0; arm_texture(&p); disarm_texture(&p); disarm_texture(&p);
        CHECK(setting == 0 && heap_users == 0);
        internal_heaps = NULL;
        [child release];
        puts("PASS overlapping heap overrides restore the original setting");
    }
    return 0;
}
