#pragma once

#include "mj_model.h"
#include "vector"
#include "string"
#include "map"

class MjSim
{
public:
    MjSim();

public:
    void init();

    void controller();

public:
    static std::vector<std::string> q_names;

    static std::map<std::string, mjtNum> q_inits;

    static mjtNum *u;

    static mjtNum sim_start;

private:    
    mjtNum *tau;
};