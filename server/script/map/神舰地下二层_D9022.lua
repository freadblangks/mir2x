local addmon = require('map.addmonster')
local addMonCo = addmon.monGener( -- 神舰地下二层_D9022
{
    {
        name = '恶形鬼',
        loc = {
            {x = 100, y = 100, w = 100, h = 100, count = 15, time = 600},
        }
    },
    {
        name = '恶形鬼8',
        loc = {
            {x = 100, y = 100, w = 100, h = 100, count = 3, time = 600},
        }
    },
    {
        name = '海神将领',
        loc = {
            {x = 100, y = 100, w = 100, h = 100, count = 45, time = 600},
        }
    },
    {
        name = '海神将领8',
        loc = {
            {x = 100, y = 100, w = 100, h = 100, count = 5, time = 600},
        }
    },
    {
        name = '爆毒神魔',
        loc = {
            {x = 100, y = 100, w = 100, h = 100, count = 35, time = 600},
        }
    },
    {
        name = '爆毒神魔8',
        loc = {
            {x = 100, y = 100, w = 100, h = 100, count = 3, time = 600},
        }
    },
    {
        name = '犬猴魔',
        loc = {
            {x = 100, y = 100, w = 100, h = 100, count = 20, time = 600},
        }
    },
    {
        name = '犬猴魔8',
        loc = {
            {x = 100, y = 100, w = 100, h = 100, count = 3, time = 600},
        }
    },
    {
        name = '神舰守卫',
        loc = {
            {x = 100, y = 100, w = 100, h = 100, count = 35, time = 600},
        }
    },
    {
        name = '神舰守卫8',
        loc = {
            {x = 100, y = 100, w = 100, h = 100, count = 3, time = 600},
        }
    },
    {
        name = '红衣法师',
        loc = {
            {x = 100, y = 100, w = 100, h = 100, count = 35, time = 600},
        }
    },
    {
        name = '红衣法师8',
        loc = {
            {x = 100, y = 100, w = 100, h = 100, count = 3, time = 600},
        }
    },
    {
        name = '触角神魔',
        loc = {
            {x = 100, y = 100, w = 100, h = 100, count = 30, time = 600},
        }
    },
    {
        name = '触角神魔8',
        loc = {
            {x = 100, y = 100, w = 100, h = 100, count = 3, time = 600},
        }
    },
    {
        name = '轻甲守卫',
        loc = {
            {x = 100, y = 100, w = 100, h = 100, count = 25, time = 600},
        }
    },
    {
        name = '轻甲守卫8',
        loc = {
            {x = 100, y = 100, w = 100, h = 100, count = 3, time = 600},
        }
    },
    {
        name = '霸王守卫',
        loc = {
            {x = 100, y = 100, w = 100, h = 100, count = 1, time = 7200},
        }
    },
})

function main()
    while true do
        local rc, errMsg = coroutine.resume(addMonCo)
        if not rc then
            fatalPrintf('addMonCo failed: %s', argDefault(errMsg, 'unknown error'))
        end
        pause(1000 * 5)
    end
end
