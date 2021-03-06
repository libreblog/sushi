/*
 * Copyright 2017-2019 Modern Ancient Instruments Networked AB, dba Elk
 *
 * SUSHI is free software: you can redistribute it and/or modify it under the terms of
 * the GNU Affero General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * SUSHI is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 * PURPOSE.  See the GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License along with
 * SUSHI.  If not, see http://www.gnu.org/licenses/
 */

/**
 * @Brief LV2 plugin control class - internally used, for holding the data of LV2 plugin controls.
 * @copyright 2017-2019 Modern Ancient Instruments Networked AB, dba Elk, Stockholm
 */

#ifndef SUSHI_LV2_CONTROL_H
#define SUSHI_LV2_CONTROL_H

#ifdef SUSHI_BUILD_WITH_LV2

#include "lv2_model.h"

namespace sushi {
namespace lv2 {

struct ScalePoint
{
    float value;
    std::string label;
};

/** Type of plugin control. */
enum class ControlType
{
    PORT, // Control port
    PROPERTY // Property (set via atom message)
};

/** Plugin control. */
class ControlID
{
public:
    ~ControlID() = default;

    static ControlID new_port_control(Port* port, Model* model, uint32_t index);
    static ControlID new_property_control(Model* model, const LilvNode* property);

    static bool has_range(Model* model, const LilvNode* subject, const char* range_uri);

    Model* model;
    ControlType type;
    LilvNode* node;
    LilvNode* symbol;
    LilvNode* label;
    LV2_URID property; // Iff type == PROPERTY
    int index; // Iff type == PORT
    LilvNode* group {nullptr}; // Port/control group, or NULL

    std::vector<ScalePoint> scale_points;
    LV2_URID value_type;
    LilvNode* min;
    LilvNode* max;
    LilvNode* def;

    bool is_toggle;
    bool is_integer;
    bool is_enumeration;
    bool is_logarithmic;
    bool is_writable; // Writable (input)
    bool is_readable; // Readable (output)

private:
    ControlID() {}
};

} // end namespace lv2
} // end namespace sushi

#endif //SUSHI_BUILD_WITH_LV2

#endif //SUSHI_LV2_CONTROL_H
