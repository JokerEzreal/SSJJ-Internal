#pragma once

namespace unity {

struct Vector2 {
    float x, y;
    Vector2() : x(0), y(0) {}
    Vector2(float x, float y) : x(x), y(y) {}
};

struct Vector3 {
    float x, y, z;
    Vector3() : x(0), y(0), z(0) {}
    Vector3(float x, float y, float z) : x(x), y(y), z(z) {}
};

struct Rect {
    float x, y, width, height;
    Rect() : x(0), y(0), width(0), height(0) {}
    Rect(float x, float y, float w, float h) : x(x), y(y), width(w), height(h) {}
};

struct Color {
    float r, g, b, a;
    Color() : r(1), g(1), b(1), a(1) {}
    Color(float r, float g, float b, float a = 1.0f) : r(r), g(g), b(b), a(a) {}

    static Color white()  { return {1, 1, 1, 1}; }
    static Color black()  { return {0, 0, 0, 1}; }
    static Color red()    { return {1, 0, 0, 1}; }
    static Color green()  { return {0, 1, 0, 1}; }
    static Color yellow() { return {1, 1, 0, 1}; }
    static Color cyan()   { return {0, 1, 1, 1}; }
};

} // namespace unity
