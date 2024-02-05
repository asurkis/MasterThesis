#include <iostream>
#include "tiny_gltf.h"

int main()
{
    tinygltf::Model model;
    tinygltf::TinyGLTF ctx;
    std::string err;
    std::string warn;
    bool success = ctx.LoadBinaryFromFile(&model, &err, &warn, "input.glb");
    std::cout << success << '\n' << err << '\n' << warn << std::endl;
}
