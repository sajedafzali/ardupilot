/// -*- tab-width: 4; Mode: C++; c-basic-offset: 4; indent-tabs-mode: nil -*-
/*
   This program is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/*
  support for external modules
 */

#include <stdio.h>
#include <dirent.h>
#if defined(HAVE_LIBDL)
#include <dlfcn.h>
#endif
#include <AP_Module/AP_Module.h>
#include <AP_Module/AP_Module_Structures.h>

struct AP_Module::hook_list *AP_Module::hooks[NUM_HOOKS];

const char *AP_Module::hook_names[AP_Module::NUM_HOOKS] = {
    "hook_setup_start",
    "hook_setup_complete",
    "hook_AHRS_update",
    "hook_gyro_sample",
    "hook_accel_sample",
};

/*
  scan a module for hook symbols
 */
void AP_Module::module_scan(const char *path)
{
#if defined(HAVE_LIBDL)
    void *m = dlopen(path, RTLD_NOW);
    if (m == nullptr) {
        printf("dlopen(%s) -> %s\n", path, dlerror());
        return;
    }
    bool found_hook = false;
    for (uint16_t i=0; i<NUM_HOOKS; i++) {
        void *s = dlsym(m, hook_names[i]);
        if (s != nullptr) {
            // found a hook in this module, add it to the list
            struct hook_list *h = new hook_list;
            if (h == nullptr) {
                AP_HAL::panic("Failed to allocate hook for %s", hook_names[i]);
            }
            h->next = hooks[i];
            h->symbol = s;
            hooks[i] = h;
            found_hook = true;
        }
    }
    if (!found_hook) {
        // we don't need this module
        dlclose(m);
    }
#endif
}

/*
  initialise AP_Module, looking for shared libraries in the given module path
*/
void AP_Module::init(const char *module_path)
{
    // scan through module directory looking for *.so
    DIR *d;
    struct dirent *de;
    d = opendir(module_path);
    if (d == nullptr) {
        return;
    }
    while ((de = readdir(d))) {
        const char *extension = strrchr(de->d_name, '.');
        if (extension == nullptr || strcmp(extension, ".so") != 0) {
            continue;
        }
        char *path = nullptr;
        if (asprintf(&path, "%s/%s", module_path, de->d_name) == -1) {
            continue;
        }
        module_scan(path);
        free(path);
    }
    closedir(d);
}


/*
  call any setup_start hooks
*/
void AP_Module::call_hook_setup_start(void)
{
    uint64_t now = AP_HAL::micros64();
    for (const struct hook_list *h=hooks[HOOK_SETUP_START]; h; h=h->next) {
        hook_setup_start_fn_t fn = reinterpret_cast<hook_setup_start_fn_t>(h->symbol);
        fn(now);
    }
}

/*
  call any setup_complete hooks
*/
void AP_Module::call_hook_setup_complete(void)
{
    uint64_t now = AP_HAL::micros64();
    for (const struct hook_list *h=hooks[HOOK_SETUP_COMPLETE]; h; h=h->next) {
        hook_setup_complete_fn_t fn = reinterpret_cast<hook_setup_complete_fn_t>(h->symbol);
        fn(now);
    }    
}

/*
  call any AHRS_update hooks
*/
void AP_Module::call_hook_AHRS_update(const AP_AHRS_NavEKF &ahrs)
{
    if (hooks[HOOK_AHRS_UPDATE] == nullptr) {
        // avoid filling in AHRS_state
        return;
    }

    /*
      construct AHRS_state structure
     */
    struct AHRS_state state {};
    state.structure_version = AHRS_state_version;
    state.time_us = AP_HAL::micros64();

    if (!ahrs.initialised()) {
        state.status = AHRS_STATUS_INITIALISING;
    } else if (ahrs.healthy()) {
        state.status = AHRS_STATUS_HEALTHY;
    } else {
        state.status = AHRS_STATUS_UNHEALTHY;
    }

    Quaternion q;
    q.from_rotation_matrix(ahrs.get_rotation_body_to_ned());
    state.quat[0] = q[0];
    state.quat[1] = q[1];
    state.quat[2] = q[2];
    state.quat[3] = q[3];

    state.eulers[0] = ahrs.roll;
    state.eulers[1] = ahrs.pitch;
    state.eulers[2] = ahrs.yaw;

    Location loc;
    if (ahrs.get_origin(loc)) {
        state.origin.initialised = true;
        state.origin.latitude = loc.lat;
        state.origin.longitude = loc.lng;
        state.origin.altitude = loc.alt*0.01f;
    }

    if (ahrs.get_position(loc)) {
        state.position.available = true;
        state.position.latitude = loc.lat;
        state.position.longitude = loc.lng;
        state.position.altitude = loc.alt*0.01f;
    }
    
    Vector3f pos;
    if (ahrs.get_relative_position_NED(pos)) {
        state.relative_position[0] = pos[0];
        state.relative_position[1] = pos[1];
        state.relative_position[2] = pos[2];
    }

    const Vector3f &gyro = ahrs.get_gyro();
    state.gyro_rate[0] = gyro[0];
    state.gyro_rate[1] = gyro[1];
    state.gyro_rate[2] = gyro[2];

    const Vector3f &accel_ef = ahrs.get_accel_ef();
    state.accel_ef[0] = accel_ef[0];
    state.accel_ef[1] = accel_ef[0];
    state.accel_ef[2] = accel_ef[0];
    
    for (const struct hook_list *h=hooks[HOOK_AHRS_UPDATE]; h; h=h->next) {
        hook_AHRS_update_fn_t fn = reinterpret_cast<hook_AHRS_update_fn_t>(h->symbol);
        fn(&state);
    }    
}


/*
  call any gyro_sample hooks
*/
void AP_Module::call_hook_gyro_sample(uint8_t instance, float dt, const Vector3f &gyro)
{
    if (hooks[HOOK_GYRO_SAMPLE] == nullptr) {
        // avoid filling in struct
        return;
    }

    /*
      construct gyro_sample structure
     */
    struct gyro_sample state {};
    state.structure_version = gyro_sample_version;
    state.time_us = AP_HAL::micros64();
    state.instance = instance;
    state.delta_time = dt;
    state.gyro[0] = gyro[0];
    state.gyro[1] = gyro[1];
    state.gyro[2] = gyro[2];

    for (const struct hook_list *h=hooks[HOOK_GYRO_SAMPLE]; h; h=h->next) {
        hook_gyro_sample_fn_t fn = reinterpret_cast<hook_gyro_sample_fn_t>(h->symbol);
        fn(&state);
    }    
}

/*
  call any accel_sample hooks
*/
void AP_Module::call_hook_accel_sample(uint8_t instance, float dt, const Vector3f &accel)
{
    if (hooks[HOOK_ACCEL_SAMPLE] == nullptr) {
        // avoid filling in struct
        return;
    }

    /*
      construct accel_sample structure
     */
    struct accel_sample state {};
    state.structure_version = accel_sample_version;
    state.time_us = AP_HAL::micros64();
    state.instance = instance;
    state.delta_time = dt;
    state.accel[0] = accel[0];
    state.accel[1] = accel[1];
    state.accel[2] = accel[2];

    for (const struct hook_list *h=hooks[HOOK_ACCEL_SAMPLE]; h; h=h->next) {
        hook_accel_sample_fn_t fn = reinterpret_cast<hook_accel_sample_fn_t>(h->symbol);
        fn(&state);
    }    
}