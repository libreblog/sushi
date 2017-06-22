#ifndef SUSHI_VST3X_HOST_CONTEXT_H
#define SUSHI_VST3X_HOST_CONTEXT_H

#define DEVELOPMENT true

#include <pluginterfaces/vst/ivsteditcontroller.h>
#include "pluginterfaces/vst/ivsthostapplication.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "public.sdk/source/vst/hosting/module.h"
#include "public.sdk/source/vst/hosting/hostclasses.h"
#undef DEVELOPMENT


namespace sushi {
namespace vst3 {

class SushiHostApplication : public Steinberg::Vst::HostApplication
{
public:
    virtual Steinberg::tresult getName (Steinberg::Vst::String128 name) override;

    /*virtual Steinberg::tresult createInstance (Steinberg::TUID cid,
                                               Steinberg::TUID _iid,
                                               void **obj) override;*/
};

//class PluginLoader;
/**
 * @brief Container to hold plugin modules and manage their lifetimes
 */
class PluginInstance
{
public:
    friend class PluginLoader;

    const std::string& name() {return _name;};
    Steinberg::Vst::IComponent* component() {return _component.get();}
    Steinberg::Vst::IAudioProcessor* processor() {return _processor.get();}
    Steinberg::Vst::IEditController* controller() {return _controller.get();}

private:
    std::string _name;
    Steinberg::IPtr<Steinberg::Vst::IComponent> _component{nullptr};
    Steinberg::IPtr<Steinberg::Vst::IAudioProcessor> _processor{nullptr};
    Steinberg::IPtr<Steinberg::Vst::IEditController> _controller{nullptr};
};

class PluginLoader
{
public:
    PluginLoader(const std::string& plugin_absolute_path);
    ~PluginLoader ();

    std::pair<bool, PluginInstance> load_plugin();

private:
    std::string _path;

    std::shared_ptr<VST3::Hosting::Module> _module{nullptr};
    SushiHostApplication _host_app;
};

Steinberg::Vst::IComponent* load_component(Steinberg::IPluginFactory* factory, std::string* name);
Steinberg::Vst::IAudioProcessor* load_processor(Steinberg::Vst::IComponent* component);
Steinberg::Vst::IEditController* load_controller(Steinberg::IPluginFactory* factory, Steinberg::Vst::IComponent*);

} // end namespace vst
} // end namespace sushi
#endif //SUSHI_VST3X_HOST_CONTEXT_H
