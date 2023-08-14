/**
    Frostbyte kernel and operating system
    Copyright (C) 2023  Amol Dhamale <amoldhamale1105@gmail.com>

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "flib.h"

int main(int argc, char** argv)
{
    if (argc > 1){
        for(int i = 0; i < argc; i++)
        {
            printf("arg%d: %s\n", i, argv[i]);
        }
        /* An exit test code passed as first argument */
        if (memcmp(argv[1], "ret", 3) == 0)
            return 0;
    }
    while (1)
    {
        msleep(10);
    }
    
    return 0;
}