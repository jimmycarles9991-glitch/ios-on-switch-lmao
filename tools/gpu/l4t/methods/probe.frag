#version 450
#ifdef LOCK_MODE
#extension GL_ARB_fragment_shader_interlock : require
layout(LOCK_MODE) in;
#endif
#ifdef POST
#extension GL_ARB_post_depth_coverage : require
layout(post_depth_coverage) in;
#endif
#ifdef EARLY
layout(early_fragment_tests) in;
#endif
layout(location=0) out vec4 color;
layout(binding=0,std430) coherent buffer Counters { uint words[]; };
void main(){
    uint index=uint(gl_FragCoord.x)+16u*uint(gl_FragCoord.y);
#ifdef LOCK_MODE
    beginInvocationInterlockARB();
#endif
    uint v=words[index]+uint(gl_SampleMaskIn[0]);
    words[index]=v;
#ifdef LOCK_MODE
    endInvocationInterlockARB();
#endif
    color=vec4(float(v&255u)/255.0,0.0,0.0,1.0);
}
