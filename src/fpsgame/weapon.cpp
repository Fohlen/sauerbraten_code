// weapon.cpp: all shooting and effects code, projectile management
#include "cube.h"
#include "game.h"

namespace game
{
    static const int MONSTERDAMAGEFACTOR = 4;
    static const int OFFSETMILLIS = 500;
    vec sg[SGRAYS];

    struct hitmsg
    {
        int target, lifesequence, info;
        ivec dir;
    };
    vector<hitmsg> hits;

    VARP(maxdebris, 10, 25, 1000);
    VARP(maxbarreldebris, 5, 10, 1000);

    ICOMMAND(getweapon, "", (), intret(player1->gunselect));

    void gunselect(int gun)
    {
        if(gun!=player1->gunselect)
        {
            addmsg(SV_GUNSELECT, "ri", gun);
            playsound(S_WEAPLOAD, &player1->o);
        }
        player1->gunselect = gun;
    }

    void nextweapon(int dir, bool force = false)
    {
        if(player1->state!=CS_ALIVE) return;
        dir = (dir < 0 ? NUMGUNS-1 : 1);
        int gun = player1->gunselect;
        loopi(NUMGUNS)
        {
            gun = (gun + dir)%NUMGUNS;
            if(force || player1->ammo[gun]) break;
        }
        if(gun != player1->gunselect) gunselect(gun);
        else playsound(S_NOAMMO);
    }
    ICOMMAND(nextweapon, "ii", (int *dir, int *force), nextweapon(*dir, *force!=0));

    void setweapon(int gun, bool force = false)
    {
        if(player1->state!=CS_ALIVE || gun<GUN_FIST || gun>GUN_PISTOL) return;
        if(force || player1->ammo[gun]) gunselect(gun);
        else playsound(S_NOAMMO);
    }
    ICOMMAND(setweapon, "ii", (int *gun, int *force), setweapon(*gun, *force!=0));

    void cycleweapon(int numguns, int *guns, bool force = false)
    {
        if(numguns<=0 || player1->state!=CS_ALIVE) return;
        int offset = 0;
        loopi(numguns) if(guns[i] == player1->gunselect) { offset = i+1; break; }
        loopi(numguns)
        {
            int gun = guns[(i+offset)%numguns];
            if(gun>=0 && gun<NUMGUNS && (force || player1->ammo[gun]))
            {
                gunselect(gun);
                return;
            }
        }
        playsound(S_NOAMMO);
    }
    ICOMMAND(cycleweapon, "sssssss", (char *w1, char *w2, char *w3, char *w4, char *w5, char *w6, char *w7),
    {
         int numguns = 0;
         int guns[7];
         if(w1[0]) guns[numguns++] = atoi(w1);
         if(w2[0]) guns[numguns++] = atoi(w2);
         if(w3[0]) guns[numguns++] = atoi(w3);
         if(w4[0]) guns[numguns++] = atoi(w4);
         if(w5[0]) guns[numguns++] = atoi(w5);
         if(w6[0]) guns[numguns++] = atoi(w6);
         if(w7[0]) guns[numguns++] = atoi(w7);
         cycleweapon(numguns, guns);
    });

    void weaponswitch()
    {
        if(player1->state!=CS_ALIVE) return;
        const int *ammo = player1->ammo;
        int s = player1->gunselect;
        if     (s!=GUN_CG     && ammo[GUN_CG])     s = GUN_CG;
        else if(s!=GUN_RL     && ammo[GUN_RL])     s = GUN_RL;
        else if(s!=GUN_SG     && ammo[GUN_SG])     s = GUN_SG;
        else if(s!=GUN_RIFLE  && ammo[GUN_RIFLE])  s = GUN_RIFLE;
        else if(s!=GUN_GL     && ammo[GUN_GL])     s = GUN_GL;
        else if(s!=GUN_PISTOL && ammo[GUN_PISTOL]) s = GUN_PISTOL;
        else                                       s = GUN_FIST;

        gunselect(s);
    }

    #define TRYWEAPON(w) do { \
        if(w[0]) \
        { \
            int gun = atoi(w); \
            if(gun >= GUN_FIST && gun <= GUN_PISTOL && gun != player1->gunselect && player1->ammo[gun]) { gunselect(gun); return; } \
        } \
        else { weaponswitch(); return; } \
    } while(0)
    ICOMMAND(weapon, "sssssss", (char *w1, char *w2, char *w3, char *w4, char *w5, char *w6, char *w7),
    {
        if(player1->state!=CS_ALIVE) return;
        TRYWEAPON(w1);
        TRYWEAPON(w2);
        TRYWEAPON(w3);
        TRYWEAPON(w4);
        TRYWEAPON(w5);
        TRYWEAPON(w6);
        TRYWEAPON(w7);
        playsound(S_NOAMMO);
    });

    void offsetray(const vec &from, const vec &to, int spread, vec &dest)
    {
        float f = to.dist(from)*spread/1000;
        for(;;)
        {
            #define RNDD rnd(101)-50
            vec v(RNDD, RNDD, RNDD);
            if(v.magnitude()>50) continue;
            v.mul(f);
            v.z /= 2;
            dest = to;
            dest.add(v);
            vec dir = dest;
            dir.sub(from);
            dir.normalize();
            raycubepos(from, dir, dest, 0, RAY_CLIPMAT|RAY_ALPHAPOLY);
            return;
        }
    }

    void createrays(const vec &from, const vec &to)             // create random spread of rays for the shotgun
    {
        loopi(SGRAYS) offsetray(from, to, SGSPREAD, sg[i]);
    }

    enum { BNC_GRENADE, BNC_GIBS, BNC_DEBRIS, BNC_BARRELDEBRIS };

    struct bouncent : physent
    {
        int lifetime;
        float lastyaw, roll;
        bool local;
        fpsent *owner;
        int bouncetype;
        vec offset;
        int offsetmillis;
        int id;
        entitylight light;
    };

    vector<bouncent *> bouncers;

    vec hudgunorigin(int gun, const vec &from, const vec &to, fpsent *d);

    void newbouncer(const vec &from, const vec &to, bool local, fpsent *owner, int type, int lifetime, int speed, entitylight *light = NULL)
    {
        bouncent &bnc = *(bouncers.add(new bouncent));
        bnc.reset();
        bnc.type = ENT_BOUNCE;
        bnc.o = from;
        bnc.radius = type==BNC_DEBRIS ? 0.5f : 1.5f;
        bnc.eyeheight = bnc.radius;
        bnc.aboveeye = bnc.radius;
        bnc.lifetime = lifetime;
        bnc.roll = 0;
        bnc.local = local;
        bnc.owner = owner;
        bnc.bouncetype = type;
        bnc.id = lastmillis;
        if(light) bnc.light = *light;

        vec dir(to);
        dir.sub(from).normalize();
        bnc.vel = dir;
        bnc.vel.mul(speed);

        avoidcollision(&bnc, dir, owner, 0.1f);

        if(type==BNC_GRENADE)
        {
            bnc.offset = hudgunorigin(GUN_GL, from, to, owner);
            if(owner==hudplayer() && !isthirdperson()) bnc.offset.sub(owner->o).rescale(16).add(owner->o);
        }
        else bnc.offset = from; 
        bnc.offset.sub(bnc.o);
        bnc.offsetmillis = OFFSETMILLIS;

        bnc.resetinterp();
    }

    void updatebouncers(int time)
    {
        loopv(bouncers)
        {
            bouncent &bnc = *bouncers[i];
            if(bnc.bouncetype==BNC_GRENADE && bnc.vel.magnitude() > 50.0f) 
            {
                vec pos(bnc.o);
                pos.add(vec(bnc.offset).mul(bnc.offsetmillis/float(OFFSETMILLIS)));
                regular_particle_splash(PART_SMOKE, 1, 150, pos, 0x404040, 2.4f, 50, -20);
            }
            vec old(bnc.o);
            bool stopped = false;
            if(bnc.bouncetype==BNC_GRENADE) stopped = bounce(&bnc, 0.6f, 0.5f) || (bnc.lifetime -= time)<0;
            else
            {
                // cheaper variable rate physics for debris, gibs, etc.
                for(int rtime = time; rtime > 0;)
                {
                    int qtime = min(30, rtime);
                    rtime -= qtime;
                    if((bnc.lifetime -= qtime)<0 || bounce(&bnc, qtime/1000.0f, 0.6f, 0.5f)) { stopped = true; break; }
                }
            }
            if(stopped)
            {
                if(bnc.bouncetype==BNC_GRENADE)
                {
                    int qdam = guns[GUN_GL].damage*(bnc.owner->quadmillis ? 4 : 1);
                    hits.setsizenodelete(0);
                    explode(bnc.local, bnc.owner, bnc.o, NULL, qdam, GUN_GL);                    
                    adddecal(DECAL_SCORCH, bnc.o, vec(0, 0, 1), RL_DAMRAD/2);
                    if(bnc.local)
                        addmsg(SV_EXPLODE, "ri3iv", lastmillis-maptime, GUN_GL, bnc.id-maptime,
                                hits.length(), hits.length()*sizeof(hitmsg)/sizeof(int), hits.getbuf());
                }
                delete &bnc;
                bouncers.remove(i--);
            }
            else
            {
                bnc.roll += old.sub(bnc.o).magnitude()/(4*RAD);
                bnc.offsetmillis = max(bnc.offsetmillis-time, 0);
            }
        }
    }

    void removebouncers(fpsent *owner)
    {
        loopv(bouncers) if(bouncers[i]->owner==owner) { delete bouncers[i]; bouncers.remove(i--); }
    }

    void clearbouncers() { bouncers.deletecontentsp(); }

    struct projectile
    {
        vec dir, o, to, offset;
        float speed;
        fpsent *owner;
        int gun;
        bool local;
        int offsetmillis;
        int id;
        entitylight light;
    };
    vector<projectile> projs;

    void clearprojectiles() { projs.setsize(0); }

    void newprojectile(const vec &from, const vec &to, float speed, bool local, fpsent *owner, int gun)
    {
        projectile &p = projs.add();
        p.dir = vec(to).sub(from).normalize();
        p.o = from;
        p.to = to;
        p.offset = hudgunorigin(gun, from, to, owner);
        p.offset.sub(from);
        p.speed = speed;
        p.local = local;
        p.owner = owner;
        p.gun = gun;
        p.offsetmillis = OFFSETMILLIS;
        p.id = lastmillis;
    }

    void removeprojectiles(fpsent *owner) 
    { 
        // can't use loopv here due to strange GCC optimizer bug
        int len = projs.length();
        loopi(len) if(projs[i].owner==owner) { projs.remove(i--); len--; }
    }

    VARP(blood, 0, 1, 1);

    void damageeffect(int damage, fpsent *d, bool thirdperson)
    {
        vec p = d->o;
        p.z += 0.6f*(d->eyeheight + d->aboveeye) - d->eyeheight;
        if(blood) particle_splash(PART_BLOOD, damage/10, 1000, p, 0x60FFFF, 2.96f);
        if(thirdperson)
        {
            s_sprintfd(ds)("@%d", damage);
            particle_text(d->abovehead(), ds, PART_TEXT, 2000, 0xFF4B19, 4.0f, -8);
        }
    }
    
    void spawnbouncer(const vec &p, const vec &vel, fpsent *d, int type, entitylight *light = NULL)
    {
        vec to(rnd(100)-50, rnd(100)-50, rnd(100)-50);
        to.normalize();
        to.add(p);
        newbouncer(p, to, true, d, type, rnd(1000)+1000, rnd(100)+20, light);
    }    

    void superdamageeffect(const vec &vel, fpsent *d)
    {
        if(!d->superdamage) return;
        vec from = d->abovehead();
        from.y -= 16;
        loopi(min(d->superdamage/25, 40)+1) spawnbouncer(from, vel, d, BNC_GIBS);
    }

    void hit(int damage, dynent *d, fpsent *at, const vec &vel, int gun, int info = 1)
    {
        if(at==player1 && d!=player1) lasthit = lastmillis;

        if(d->type==ENT_INANIMATE) 
        {
            hitmovable(damage, (movable *)d, at, vel, gun);
            return;
        }

        fpsent *f = (fpsent *)d;

        f->lastpain = lastmillis;
        if(at->type==ENT_PLAYER) at->totaldamage += damage;
        f->superdamage = 0;

        if(f->type==ENT_AI || !m_mp(gamemode) || f==player1) f->hitpush(damage, vel, at, gun);

        if(f->type==ENT_AI) hitmonster(damage, (monster *)f, at, vel, gun);
        else if(!m_mp(gamemode)) damaged(damage, f, at);
        else 
        { 
            hitmsg &h = hits.add();
            h.target = f->clientnum;
            h.lifesequence = f->lifesequence;
            h.info = info;
            damageeffect(damage, f);
            if(f==player1)
            {
                h.dir = ivec(0, 0, 0);
                damageblend(damage);
                damagecompass(damage, at ? at->o : f->o);
                playsound(S_PAIN6);
            }
            else 
            {
                h.dir = ivec(int(vel.x*DNF), int(vel.y*DNF), int(vel.z*DNF));
                playsound(S_PAIN1+rnd(5), &f->o); 
            }
        }
    }

    void hitpush(int damage, dynent *d, fpsent *at, vec &from, vec &to, int gun, int rays)
    {
        vec v(to);
        v.sub(from);
        v.normalize();
        hit(damage, d, at, v, gun, rays);
    }

    float projdist(dynent *o, vec &dir, const vec &v)
    {
        vec middle = o->o;
        middle.z += (o->aboveeye-o->eyeheight)/2;
        float dist = middle.dist(v, dir);
        dir.div(dist);
        if(dist<0) dist = 0;
        return dist;
    }

    void radialeffect(dynent *o, const vec &v, int qdam, fpsent *at, int gun)
    {
        if(o->state!=CS_ALIVE) return;
        vec dir;
        float dist = projdist(o, dir, v);
        if(dist<RL_DAMRAD) 
        {
            int damage = (int)(qdam*(1-dist/RL_DISTSCALE/RL_DAMRAD));
            if(gun==GUN_RL && o==at) damage /= RL_SELFDAMDIV; 
            hit(damage, o, at, dir, gun, int(dist*DMF));
        }
    }

    void explode(bool local, fpsent *owner, const vec &v, dynent *safe, int damage, int gun)
    {
        particle_splash(PART_SPARK, 200, 300, v, 0xB49B4B, 0.24f);
        playsound(S_RLHIT, &v);
        particle_fireball(v, RL_DAMRAD, PART_EXPLOSION, -1, gun==GUN_RL ? 0xFF8080 : 0xA0C080, 4.0f);
        if(gun==GUN_RL) adddynlight(v, 1.15f*RL_DAMRAD, vec(2, 1.5f, 1), 900, 100, 0, RL_DAMRAD/2, vec(1, 0.75f, 0.5f));
        else if(gun==GUN_GL) adddynlight(v, 1.15f*RL_DAMRAD, vec(2, 1.5f, 1), 900, 100, 0, 8, vec(0.25f, 1, 1));
        else adddynlight(v, 1.15f*RL_DAMRAD, vec(2, 1.5f, 1), 900, 100);
        int numdebris = gun==GUN_BARREL ? rnd(max(maxbarreldebris-5, 1))+5 : rnd(maxdebris-5)+5;
        vec debrisvel = owner->o==v ? vec(0, 0, 0) : vec(owner->o).sub(v).normalize(), debrisorigin(v);
        if(gun==GUN_RL) debrisorigin.add(vec(debrisvel).mul(8));
        if(numdebris)
        {
            entitylight light;
            lightreaching(debrisorigin, light.color, light.dir);
            loopi(numdebris)
                spawnbouncer(debrisorigin, debrisvel, owner, gun==GUN_BARREL ? BNC_BARRELDEBRIS : BNC_DEBRIS, &light);
        }
        if(!local) return;
        loopi(numdynents())
        {
            dynent *o = iterdynents(i);
            if(!o || o==safe) continue;
            radialeffect(o, v, damage, owner, gun);
        }
    }

    void projsplash(projectile &p, vec &v, dynent *safe, int damage)
    {
        if(guns[p.gun].part)
        {
            particle_splash(PART_SPARK, 100, 200, v, 0xB49B4B, 0.24f);
            playsound(S_FEXPLODE, &v);
            // no push?
        }
        else
        {
            explode(p.local, p.owner, v, safe, damage, GUN_RL);
            adddecal(DECAL_SCORCH, v, vec(p.dir).neg(), RL_DAMRAD/2);
        }
    }

    bool projdamage(dynent *o, projectile &p, vec &v, int qdam)
    {
        if(o->state!=CS_ALIVE) return false;
        if(!intersect(o, p.o, v)) return false;
        projsplash(p, v, o, qdam);
        vec dir;
        projdist(o, dir, v);
        hit(qdam, o, p.owner, dir, p.gun, 0);
        return true;
    }

    void updateprojectiles(int time)
    {
        loopv(projs)
        {
            projectile &p = projs[i];
            p.offsetmillis = max(p.offsetmillis-time, 0);
            int qdam = guns[p.gun].damage*(p.owner->quadmillis ? 4 : 1);
            if(p.owner->type==ENT_AI) qdam /= MONSTERDAMAGEFACTOR;
            vec v;
            float dist = p.to.dist(p.o, v);
            float dtime = dist*1000/p.speed; 
            if(time > dtime) dtime = time;
            v.mul(time/dtime);
            v.add(p.o);
            bool exploded = false;
            hits.setsizenodelete(0);
            if(p.local)
            {
                loopj(numdynents())
                {
                    dynent *o = iterdynents(j);
                    if(!o || p.owner==o || o->o.reject(v, 10.0f)) continue;
                    if(projdamage(o, p, v, qdam)) { exploded = true; break; }
                }
            }
            if(!exploded)
            {
                if(dist<4)
                {
                    if(p.o!=p.to) // if original target was moving, reevaluate endpoint
                    {
                        if(raycubepos(p.o, p.dir, p.to, 0, RAY_CLIPMAT|RAY_ALPHAPOLY)>=4) continue;
                    }
                    projsplash(p, v, NULL, qdam);
                    exploded = true;
                }
                else 
                {   
                    vec pos(v);
                    pos.add(vec(p.offset).mul(p.offsetmillis/float(OFFSETMILLIS)));
                    if(guns[p.gun].part)
                    {
                         regular_particle_splash(PART_SMOKE, 2, 300, pos, 0x404040, 0.6f, 150, -20);
                         int color = 0xFFFFFF;
                         switch(guns[p.gun].part)
                         {
                            case PART_FIREBALL1: color = 0xFFC8C8; break;
                         }
                         particle_splash(guns[p.gun].part, 1, 1, pos, color, 4.8f, 150, 20);
                    }
                    else regular_particle_splash(PART_SMOKE, 2, 300, pos, 0x404040, 2.4f, 50, -20);
                }   
            }
            if(exploded) 
            {
                if(p.local)
                    addmsg(SV_EXPLODE, "ri3iv", lastmillis-maptime, p.gun, p.id-maptime,
                            hits.length(), hits.length()*sizeof(hitmsg)/sizeof(int), hits.getbuf());
                projs.remove(i--);
            }
            else p.o = v;
        }
    }

    extern int chainsawhudgun;

    void shoteffects(int gun, const vec &from, const vec &to, fpsent *d, bool local)     // create visual effect from a shot
    {
        int sound = guns[gun].sound, pspeed = 25;
        switch(gun)
        {
            case GUN_FIST:
                if(d->type==ENT_PLAYER && chainsawhudgun) sound = S_CHAINSAW_ATTACK;
                break;

            case GUN_SG:
            {
                if(!local) createrays(from, to);
                if(d->muzzle.x >= 0)
                    particle_flare(d->muzzle, d->muzzle, 200, PART_MUZZLE_FLASH, 0xFFFFFF, 1.0f, d);
                loopi(SGRAYS)
                {
                    particle_splash(PART_SPARK, 20, 250, sg[i], 0xB49B4B, 0.24f);
                    particle_flare(hudgunorigin(gun, from, sg[i], d), sg[i], 300, PART_STREAK, 0xFFC864, 0.28f);
                    if(!local) adddecal(DECAL_BULLET, sg[i], vec(from).sub(sg[i]).normalize(), 2.0f);
                }
                break;
            }

            case GUN_CG:
            case GUN_PISTOL:
            {
                particle_splash(PART_SPARK, 200, 250, to, 0xB49B4B, 0.24f);
                particle_flare(hudgunorigin(gun, from, to, d), to, 600, PART_STREAK, 0xFFC864, 0.28f);
                if(d->muzzle.x >= 0)
                    particle_flare(d->muzzle, d->muzzle, gun==GUN_CG ? 100 : 200, PART_MUZZLE_FLASH, 0xFFFFFF, gun==GUN_CG ? 0.75f : 0.5f, d);
                if(!local) adddecal(DECAL_BULLET, to, vec(from).sub(to).normalize(), 2.0f);
                //if(gun==GUN_CG) adddynlight(hudgunorigin(gun, d->o, to, d), 30, vec(1, 0.75f, 0.5f), 50, 0, DL_FLASH); 
                break;
            }

            case GUN_RL:
                if(d->muzzle.x >= 0)
                    particle_flare(d->muzzle, d->muzzle, 300, PART_MUZZLE_FLASH, 0xFFFFFF, 1.5f, d);
            case GUN_FIREBALL:
            case GUN_ICEBALL:
            case GUN_SLIMEBALL:
                pspeed = guns[gun].projspeed*4;
                if(d->type==ENT_AI) pspeed /= 2;
                newprojectile(from, to, (float)pspeed, local, d, gun);
                break;

            case GUN_GL:
            {
                float dist = from.dist(to);
                vec up = to;
                up.z += dist/8;
                if(d->muzzle.x >= 0)
                    particle_flare(d->muzzle, d->muzzle, 200, PART_MUZZLE_FLASH, 0xFFFFFF, 0.75f, d);
                newbouncer(from, up, local, d, BNC_GRENADE, 2000, 200);
                break;
            }

            case GUN_RIFLE: 
                particle_splash(PART_SPARK, 200, 250, to, 0xB49B4B, 0.24f);
                particle_trail(PART_SMOKE, 500, hudgunorigin(gun, from, to, d), to, 0x404040, 0.6f, 20);
                if(d->muzzle.x >= 0)
                    particle_flare(d->muzzle, d->muzzle, 150, PART_MUZZLE_FLASH, 0xFFFFFF, 0.5f, d);
                if(!local) adddecal(DECAL_BULLET, to, vec(from).sub(to).normalize(), 3.0f);
                break;
        }

        if(d->attacksound >= 0 && d->attacksound != sound) d->stopattacksound();
        switch(sound)
        {
            case S_CHAINSAW_ATTACK:
                d->attacksound = sound;
                d->attackchan = playsound(sound, d==hudplayer() ? NULL : &d->o, NULL, -1, 100, d->attackchan);
                break;
            default:
                playsound(sound, d==hudplayer() ? NULL : &d->o);
                break;
        } 
    }

    bool intersect(dynent *d, const vec &from, const vec &to)   // if lineseg hits entity bounding box
    {
        float dist;
        vec bottom(d->o), top(d->o);
        bottom.z -= d->eyeheight;
        top.z += d->aboveeye;
        return linecylinderintersect(from, to, bottom, top, d->radius, dist);
    }

    dynent *intersectclosest(const vec &from, const vec &to, fpsent *at)
    {
        dynent *best = NULL;
        float bestdist = 1e16f;
        loopi(numdynents())
        {
            dynent *o = iterdynents(i);
            if(!o || o==at || o->state!=CS_ALIVE) continue;
            if(!intersect(o, from, to)) continue;
            float dist = at->o.dist(o->o);
            if(dist<bestdist)
            {
                best = o;
                bestdist = dist;
            }
        }
        return best;
    }

    void shorten(vec &from, vec &to, vec &target)
    {
        target.sub(from).normalize().mul(from.dist(to)).add(from);
    }

    void raydamage(vec &from, vec &to, fpsent *d)
    {
        int qdam = guns[d->gunselect].damage;
        if(d->quadmillis) qdam *= 4;
        if(d->type==ENT_AI) qdam /= MONSTERDAMAGEFACTOR;
        dynent *o, *cl;
        if(d->gunselect==GUN_SG)
        {
            bool done[SGRAYS];
            loopj(SGRAYS) done[j] = false;
            for(;;)
            {
                bool raysleft = false;
                int hitrays = 0;
                o = NULL;
                loop(r, SGRAYS) if(!done[r] && (cl = intersectclosest(from, sg[r], d)))
                {
                    if(!o || o==cl)
                    {
                        hitrays++;
                        o = cl;
                        done[r] = true;
                        shorten(from, o->o, sg[r]);
                    }
                    else raysleft = true;
                }
                if(hitrays) hitpush(hitrays*qdam, o, d, from, to, d->gunselect, hitrays);
                if(!raysleft) break;
            }
            loopj(SGRAYS) if(!done[j]) adddecal(DECAL_BULLET, sg[j], vec(from).sub(sg[j]).normalize(), 2.0f); 
        }
        else if((o = intersectclosest(from, to, d)))
        {
            hitpush(qdam, o, d, from, to, d->gunselect, 1);
            shorten(from, o->o, to);
        }
        else if(d->gunselect!=GUN_FIST && d->gunselect!=GUN_BITE) adddecal(DECAL_BULLET, to, vec(from).sub(to).normalize(), d->gunselect==GUN_RIFLE ? 3.0f : 2.0f); 
    }

    void shoot(fpsent *d, const vec &targ)
    {
        int attacktime = lastmillis-d->lastaction;
        if(attacktime<d->gunwait) return;
        d->gunwait = 0;
        if(d==player1 && !d->attacking) return;
        d->lastaction = lastmillis;
        d->lastattackgun = d->gunselect;
        if(!d->ammo[d->gunselect]) 
        { 
            if(d==player1)
            {
                msgsound(S_NOAMMO, d); 
                d->gunwait = 600; 
                d->lastattackgun = -1; 
                weaponswitch(); 
            }
            return; 
        }
        if(d->gunselect) d->ammo[d->gunselect]--;
        vec from = d->o;
        vec to = targ;

        vec unitv;
        float dist = to.dist(from, unitv);
        unitv.div(dist);
        vec kickback(unitv);
        kickback.mul(guns[d->gunselect].kickamount*-2.5f);
        d->vel.add(kickback);
        float shorten = 0;
        if(guns[d->gunselect].range && dist > guns[d->gunselect].range)
            shorten = guns[d->gunselect].range;
        float barrier = raycube(d->o, unitv, dist, RAY_CLIPMAT|RAY_ALPHAPOLY);
        if(barrier > 0 && barrier < dist && (!shorten || barrier < shorten))
            shorten = barrier;
        if(shorten)
        {
            to = unitv;
            to.mul(shorten);
            to.add(from);
        }
        
        if(d->gunselect==GUN_SG) createrays(from, to);
        else if(d->gunselect==GUN_CG) offsetray(from, to, 1, to);
            
        if(d->quadmillis && attacktime>200) msgsound(S_ITEMPUP, d);

        hits.setsizenodelete(0);

        if(!guns[d->gunselect].projspeed) raydamage(from, to, d);

        shoteffects(d->gunselect, from, to, d, true);

        if(d==player1)
        {
            addmsg(SV_SHOOT, "ri2i6iv", lastmillis-maptime, d->gunselect,
                   (int)(from.x*DMF), (int)(from.y*DMF), (int)(from.z*DMF), 
                   (int)(to.x*DMF), (int)(to.y*DMF), (int)(to.z*DMF),
                   hits.length(), hits.length()*sizeof(hitmsg)/sizeof(int), hits.getbuf());
        }

        d->gunwait = guns[d->gunselect].attackdelay;

        d->totalshots += guns[d->gunselect].damage*(d->quadmillis ? 4 : 1)*(d->gunselect==GUN_SG ? SGRAYS : 1);
    }

    void adddynlights()
    {
        loopv(projs)
        {
            projectile &p = projs[i];
            if(p.gun!=GUN_RL) continue;
            vec pos(p.o);
            pos.add(vec(p.offset).mul(p.offsetmillis/float(OFFSETMILLIS)));
            adddynlight(pos, RL_DAMRAD/2, vec(1, 0.75f, 0.5f));
        }
        loopv(bouncers)
        {
            bouncent &bnc = *bouncers[i];
            if(bnc.bouncetype!=BNC_GRENADE) continue;
            vec pos(bnc.o);
            pos.add(vec(bnc.offset).mul(bnc.offsetmillis/float(OFFSETMILLIS)));
            adddynlight(pos, 8, vec(0.25f, 1, 1));
        }
    }

    void preloadbouncers()
    {
        const char *mdls[] =
        {
            "gibc", "gibh",
            "projectiles/grenade", "projectiles/rocket",
            "debris/debris01", "debris/debris02", "debris/debris03", "debris/debris04",
            "barreldebris/debris01", "barreldebris/debris02", "barreldebris/debris03", "barreldebris/debris04"
        };
        loopi(sizeof(mdls)/sizeof(mdls[0]))
        {
            preloadmodel(mdls[i]);
        }
    }

    void renderbouncers()
    {
        float yaw, pitch;
        loopv(bouncers)
        {
            bouncent &bnc = *(bouncers[i]);
            vec pos(bnc.o);
            pos.add(vec(bnc.offset).mul(bnc.offsetmillis/float(OFFSETMILLIS)));
            vec vel(bnc.vel);
            if(vel.magnitude() <= 25.0f) yaw = bnc.lastyaw;
            else
            {
                vectoyawpitch(vel, yaw, pitch);
                yaw += 90;
                bnc.lastyaw = yaw;
            }
            pitch = -bnc.roll;
            const char *mdl = "projectiles/grenade";
            string debrisname;
            int cull = MDL_CULL_VFC|MDL_CULL_DIST|MDL_CULL_OCCLUDED;
            if(bnc.bouncetype==BNC_GIBS) { mdl = ((int)(size_t)&bnc)&0x40 ? "gibc" : "gibh"; cull |= MDL_LIGHT|MDL_DYNSHADOW; }
            else if(bnc.bouncetype==BNC_DEBRIS) { s_sprintf(debrisname)("debris/debris0%d", ((((int)(size_t)&bnc)&0xC0)>>6)+1); mdl = debrisname; }
            else if(bnc.bouncetype==BNC_BARRELDEBRIS) { s_sprintf(debrisname)("barreldebris/debris0%d", ((((int)(size_t)&bnc)&0xC0)>>6)+1); mdl = debrisname; }
            else { cull |= MDL_LIGHT|MDL_DYNSHADOW; cull &= ~MDL_CULL_DIST; }
            rendermodel(&bnc.light, mdl, ANIM_MAPMODEL|ANIM_LOOP, pos, yaw, pitch, cull);
        }
    }
    
    void renderprojectiles()
    {
        float yaw, pitch;
        loopv(projs)
        {
            projectile &p = projs[i];
            if(p.gun!=GUN_RL) continue;
            vec pos(p.o);
            pos.add(vec(p.offset).mul(p.offsetmillis/float(OFFSETMILLIS)));
            if(p.to==pos) continue;
            vec v(p.to);
            v.sub(pos);
            v.normalize();
            // the amount of distance in front of the smoke trail needs to change if the model does
            vectoyawpitch(v, yaw, pitch);
            yaw += 90;
            v.mul(3);
            v.add(pos);
            rendermodel(&p.light, "projectiles/rocket", ANIM_MAPMODEL|ANIM_LOOP, v, yaw, pitch, MDL_CULL_VFC|MDL_CULL_OCCLUDED|MDL_LIGHT);
        }
    }  

    void checkattacksound(fpsent *d)
    {
        int gun = -1;
        switch(d->attacksound)
        {
            case S_CHAINSAW_ATTACK:
                if(chainsawhudgun) gun = GUN_FIST;
                break;
            default:
                return;
        }
        if(gun >= 0 && gun < NUMGUNS &&
           d->clientnum >= 0 && d->state == CS_ALIVE &&
           d->lastattackgun == gun && lastmillis - d->lastaction < guns[gun].attackdelay + 50)
            d->attackchan = playsound(d->attacksound, d==hudplayer() ? NULL : &d->o, NULL, -1, -1, d->attackchan);
        else d->stopattacksound();
    }

    int idlesound = -1, idlechan = -1;

    void updateweapons(int curtime)
    {
        updateprojectiles(curtime);
        if(player1->clientnum>=0 && player1->state==CS_ALIVE) shoot(player1, worldpos); // only shoot when connected to server
        updatebouncers(curtime); // need to do this after the player shoots so grenades don't end up inside player's BB next frame
        checkattacksound(player1);
        loopv(players) if(players[i]) checkattacksound(players[i]);
        fpsent *d = followingplayer();
        if(!d) d = player1;
        int sound = -1;
        if(d->clientnum >= 0 && d->state == CS_ALIVE) switch(d->gunselect)
        {
            case GUN_FIST:
                if(chainsawhudgun && d->attacksound < 0) sound = S_CHAINSAW_IDLE;
                break;
        }
        if(idlesound != sound)
        {
            if(idlesound >= 0)
            {
                stopsound(idlesound, idlechan, 100);
                idlesound = idlechan = -1;
            }
            if(sound >= 0)
            {
                idlechan = playsound(sound, NULL, NULL, -1, 100, idlechan);
                if(idlechan >= 0) idlesound = sound;
            }
        }                
    }
};
