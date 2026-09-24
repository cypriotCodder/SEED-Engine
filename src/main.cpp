#include "platform/window.hpp"
#include "render/renderer.hpp"
#include "core/scene.hpp"
#include "world/world.hpp"
#include "world/player_save.hpp"
#include "physics/physics.hpp"
#include "core/particles.hpp"
#include "platform/audio.hpp"
#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <string_view>

int main(int argc,char** argv) {
    try {
        bool smoke=false;
        bool damage_demo=false,overview=false;
        bool verify_stream=false;
        const char* screenshot=nullptr;
        std::uint64_t seed=20260923;
        std::filesystem::path save="saves/island";
        for (int i=1;i<argc;++i) {
            const std::string_view arg=argv[i];
            if (arg=="--smoke") smoke=true;
            else if (arg=="--damage-demo") damage_demo=true;
            else if (arg=="--overview") overview=true;
            else if (arg=="--verify-stream") verify_stream=true;
            else if (arg=="--screenshot" && i+1<argc) screenshot=argv[++i];
            else if (arg=="--save" && i+1<argc) save=argv[++i];
            else if (arg=="--seed" && i+1<argc) {
                const std::string_view value=argv[++i];
                const auto result=std::from_chars(value.data(),value.data()+value.size(),seed);
                if (result.ec!=std::errc{} || result.ptr!=value.data()+value.size()) throw std::invalid_argument("Invalid 64-bit seed");
            } else throw std::invalid_argument("Usage: seed_demo [--smoke] [--damage-demo] [--overview] [--screenshot FILE.ppm] [--seed N] [--save DIRECTORY]");
        }
        seed::Jobs jobs;
        char* base=SDL_GetBasePath();
        if (!base) throw std::runtime_error("Cannot determine executable asset directory");
        const auto pack_path=std::filesystem::path(base)/"demo.pak"; SDL_free(base);
        seed::PackStream assets(jobs,pack_path);
        seed::Window window(!smoke);
        seed::Renderer renderer(assets.get());
        seed::Scene scene;
        seed::World world(jobs,seed,save);
        const auto spawn=seed::load_player(save,seed);
        const auto player=scene.create({spawn,spawn,0},{});
        world.settle(spawn.chunk);
        if (verify_stream) {
            world.settle({});
            world.dig({{},{10.5F,10.5F}});
            world.settle({8,8}); world.settle({});
            const auto* changed=world.tile({{},{10.5F,10.5F}});
            if (!changed || changed->material!=0 || changed->elevation>=0) throw std::runtime_error("Chunk delta unload/reload check failed");
            world.settle(spawn.chunk);
            std::puts("Chunk delta survived generation, edit, unload and reload.");
        }
        seed::Physics physics(scene,seed,save);
        seed::Particles particles;
        seed::Audio audio(!smoke);
        seed::Input input;
        auto previous=std::chrono::steady_clock::now();
        float accumulator=0;
        unsigned frames=0;
        float damage_cooldown=0;
        while (!input.quit) {
            const auto now=std::chrono::steady_clock::now();
            const float dt=smoke ? 1.0F/60.0F : std::clamp(std::chrono::duration<float>(now-previous).count(),0.0F,0.1F);
            previous=now;
            window.poll(input);
            damage_cooldown=std::max(0.0F,damage_cooldown-dt);
            if (input.pressed[SDL_SCANCODE_TAB]) overview=!overview;
            if (damage_demo && frames==10) { physics.collapse_demo(); particles.burst({{},{0,5}}); audio.impact(); }
            if (input.pressed[SDL_SCANCODE_ESCAPE]) input.quit=true;
            auto& transform=*scene.transforms.find(player);
            world.stream(transform.position.chunk);
            accumulator+=dt;
            constexpr float step=1.0F/60.0F;
            while (accumulator>=step) {
                transform.previous=transform.position;
                const seed::Vec2 movement{
                    static_cast<float>(input.held[SDL_SCANCODE_D])-static_cast<float>(input.held[SDL_SCANCODE_A]),
                    static_cast<float>(input.held[SDL_SCANCODE_W])-static_cast<float>(input.held[SDL_SCANCODE_S])};
                auto candidate=transform.position;
                candidate.move(seed::normalized(movement)*(6*step));
                const auto* ground=world.tile(candidate);
                if (ground && ground->elevation>=0 && !ground->tree && !physics.blocks(candidate)) transform.position=candidate;
                physics.step(jobs,transform.position);
                particles.update(step);
                accumulator-=step;
            }
            auto camera=transform.position;
            camera.move(seed::relative(transform.previous,transform.position)*(1-accumulator/step));
            int width{},height{},logical_width{},logical_height{};
            window.drawable_size(width,height); window.logical_size(logical_width,logical_height);
            if (width<=0 || height<=0 || logical_width<=0 || logical_height<=0) { SDL_Delay(16); continue; }
            const float zoom=(overview ? 12.0F : 40.0F)*static_cast<float>(width)/logical_width;
            if (((input.mouse_buttons & (SDL_BUTTON_LMASK|SDL_BUTTON_RMASK)) && damage_cooldown==0) || input.pressed[SDL_SCANCODE_B]) {
                auto target=camera;
                target.move({(static_cast<float>(input.mouse_x)/logical_width-0.5F)*width/zoom,
                             (0.5F-static_cast<float>(input.mouse_y)/logical_height)*height/zoom});
                if (seed::length(seed::relative(target,transform.position))<=4) {
                    if (input.pressed[SDL_SCANCODE_B]) {
                        const auto* ground=world.tile(target);
                        if (ground && ground->elevation>=0 && !ground->tree) physics.build(target);
                    } else {
                        bool hit=false;
                        if (input.mouse_buttons & SDL_BUTTON_RMASK) {
                            hit=world.dig(target); if (hit) physics.damage(target,100);
                        } else hit=physics.damage(target,35) || world.remove_tree(target);
                        if (hit) { particles.burst(target); audio.impact(); }
                        damage_cooldown=0.15F;
                    }
                }
            }
            renderer.begin(width,height,0,0,zoom);
            world.each([&](seed::ChunkCoord coord,const seed::Chunk& chunk) {
                const auto offset=seed::relative({coord,{}},camera);
                for (int y=0;y<seed::chunk_side;++y) for (int x=0;x<seed::chunk_side;++x) {
                    const auto& tile=chunk.tiles[static_cast<std::size_t>(y*seed::chunk_side+x)];
                    renderer.sprite(static_cast<seed::Material>(tile.material),offset.x+x+0.5F,offset.y+y+0.5F,
                        1,1,0,0.94F+tile.moisture*0.15F);
                }
            });
            world.each([&](seed::ChunkCoord coord,const seed::Chunk& chunk) {
                const auto offset=seed::relative({coord,{}},camera);
                for (int y=0;y<seed::chunk_side;++y) for (int x=0;x<seed::chunk_side;++x) {
                    if (!chunk.tiles[static_cast<std::size_t>(y*seed::chunk_side+x)].tree) continue;
                    const float px=offset.x+x+0.5F,py=offset.y+y+0.5F;
                    renderer.sprite(seed::Material::leaves,px+0.4F,py-0.3F,2.0F,1.3F,0,0.3F);
                    renderer.sprite(seed::Material::wood,px,py,0.35F,0.65F);
                    renderer.sprite(seed::Material::leaves,px,py+0.5F,1.8F,1.8F);
                    renderer.sprite(seed::Material::leaves,px-0.2F,py+0.9F,1.1F,1.1F,0,1.3F);
                }
            });
            const auto owners=scene.visuals.owners(); const auto visuals=scene.visuals.values();
            for (std::size_t i=0;i<owners.size();++i) {
                const auto& t=*scene.transforms.find(owners[i]);
                if (!seed::nearby(t.position.chunk,camera.chunk,3)) continue;
                const auto p=seed::relative(t.previous,camera)+seed::relative(t.position,t.previous)*(accumulator/step);
                renderer.sprite(visuals[i].material,p.x,p.y,visuals[i].size.x,visuals[i].size.y,t.angle);
            }
            particles.draw(renderer,camera);
            if (seed::nearby({},camera.chunk,3)) {
                const auto fire=seed::relative({{},{1,1}},camera);
                const float flicker=1+0.08F*std::sin(static_cast<float>(frames)*0.7F);
                renderer.sprite(seed::Material::ember,fire.x,fire.y,0.7F*flicker,1.2F*flicker);
                renderer.light(fire.x,fire.y,9,1,0.53F,0.19F,5*flicker,2.5F);
            }
            renderer.light(0,0,6,0.6F,0.72F,1,1,4);
            renderer.finish();
            if (screenshot && ((smoke && frames==59) || input.pressed[SDL_SCANCODE_F12])) renderer.screenshot(screenshot,width,height);
            window.present();
            if (frames%60==0) {
                char title[160];
                std::snprintf(title,sizeof(title),"Seed Engine | WASD | Left damage / Right dig | B build | Tab map | Esc save | %u draws",renderer.draw_calls());
                window.title(title);
            }
            ++frames;
            if (smoke && frames>=60) input.quit=true;
            if (!smoke) std::this_thread::sleep_until(previous+std::chrono::microseconds(16667));
        }
        world.save();
        physics.save();
        seed::save_player(save,seed,scene.transforms.find(player)->position);
        if (damage_demo && physics.grounded_unsupported()==0) throw std::runtime_error("Collapse check failed: no unsupported pieces reached the ground");
        std::printf("Presented %u frames; %u draw calls; %u unsupported pieces (%u grounded); changes saved.\n",
            frames,renderer.draw_calls(),physics.unsupported(),physics.grounded_unsupported());
    } catch (const std::exception& error) {
        std::fprintf(stderr,"Engine error: %s\n",error.what()); return 1;
    }
}
