// SPDX-License-Identifier: MIT
// Which stand-in a texture's draws sample and when it is current within a range (TegraGM20BMetalRangeStep.mm standIns). A view
// of an extended-range or RG8Unorm_sRGB texture in the same format samples a view of its texture's stand-in, the same
// memory; one in another transfer of the same codes samples a stand-in of its own (texture_views.cpp
// viewCompanions). Every one of them holds what the codes of one storage owner (the view's levels owner) read, so a write
// through any texture over those codes makes all of them stale.
#pragma once
#import "TegraGM20BMetalInternal.h"

#include <algorithm>
#include <utility>
#include <vector>

// The texture whose memory `t`'s texels are
inline SwitchMetalTexture *gm20bStorageOwner(SwitchMetalTexture *t)
{
    return t->_levelsOwner ?: t;
}

// The texture whose conversion makes `t`'s stand-in current: its storage owner where `t` reads the owner's transfer (its
// stand-in a view of the owner's), else `t`
inline SwitchMetalTexture *gm20bStandInHolder(SwitchMetalTexture *t)
{
    SwitchMetalTexture *o = gm20bStorageOwner(t);
    return o != t && o->_raw.standIn && t->_raw.format == o->_raw.format ? o : t;
}

// Whether a write to `t` changes codes some stand-in is drawn from
inline bool gm20bStandInBacked(SwitchMetalTexture *t)
{
    return t->_raw.standIn || gm20bStorageOwner(t)->_raw.standIn;
}

// What the conversion of `holder`'s stand-in reads and draws into: its storage owner's codes, of `holder`'s format, and the
// stand-in `holder` owns (a view's own stand-in is the one its header object holds)
inline gm20b::Texture gm20bStandInCodes(SwitchMetalTexture *holder)
{
    SwitchMetalTexture *o = gm20bStorageOwner(holder);
    if (o == holder)
        return holder->_raw;
    gm20b::Texture codes = o->_raw;
    codes.format = holder->_raw.format;
    codes.standIn = holder->_raw.standIn->standIn;
    return codes;
}

// The stand-ins drawn current in a range: one record a (storage owner, holder) pair, so a view and its texture share one
// where they share a stand-in, and a write through either drops every record of their codes
class SwitchMetalStandInFresh {
public:
    bool insert(SwitchMetalTexture *t)
    {
        if (!t || count(t))
            return false;
        held_.push_back({gm20bStorageOwner(t), t});
        return true;
    }

    size_t count(SwitchMetalTexture *t) const
    {
        return t && std::find(held_.begin(), held_.end(), std::make_pair(gm20bStorageOwner(t), t)) != held_.end();
    }

    void erase(SwitchMetalTexture *t)
    {
        if (!t)
            return;
        SwitchMetalTexture *o = gm20bStorageOwner(t);
        held_.erase(std::remove_if(held_.begin(), held_.end(),
                                   [&](const auto &r) {
                                       return r.first == o;
                                   }),
                    held_.end());
    }

    void clear()
    {
        held_.clear();
    }

    bool empty() const
    {
        return held_.empty();
    }

private:
    std::vector<std::pair<SwitchMetalTexture *, SwitchMetalTexture *>> held_;
};
