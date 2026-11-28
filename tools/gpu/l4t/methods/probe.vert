#version 450
void main(){
    vec2 p=vec2((gl_VertexIndex==1)?3:-1,(gl_VertexIndex==2)?3:-1);
    gl_Position=vec4(p,0.5,1.0);
}
