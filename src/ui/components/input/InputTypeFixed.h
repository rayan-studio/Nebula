// Fixed minimal header to provide Input::Type for inline Explorer input
#pragma once

class Input
{
public:
    enum class Type
    {
        File,
        Folder,
        Search
    };
};
