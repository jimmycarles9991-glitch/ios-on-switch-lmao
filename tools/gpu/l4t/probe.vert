#version 450
layout(location=0) in vec4 pos;
layout(location=0) out vec4 v;
void main(){gl_Position=pos; v=pos*0.5+0.5;}
