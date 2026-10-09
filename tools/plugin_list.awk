# Edit MPC.settings' plugin list (BusyBox awk).
#   awk -v mode=add    -v list=pluginList-arm -v file=/path/tributary.so -v entryfile=plugin-meta.xml -f plugin_list.awk MPC.settings
#   awk -v mode=remove -v list=pluginList-arm -v file=/path/tributary.so -f plugin_list.awk MPC.settings
# list is pluginList-arm on a 32-bit package and pluginList-arm-64bit on an AArch64 package.
# optional: -v uid=54726962   also drops entries with that uid (an old install at another path)
#           -v alt=/old/path.so  also drops entries with that file=
function flush() {
    drop = index(buf, "file=\"" file "\"") != 0
    if (!drop && alt != "" && index(buf, "file=\"" alt "\"") != 0) drop = 1
    if (!drop && uid != "" && index(buf, " uid=\"" uid "\"") != 0) drop = 1
    if (!drop) print buf
    buf = ""
}
function islist() {
    return index($0, "<VALUE name=\"" list "\">") != 0
}
BEGIN {
    if (list == "") list = "pluginList-arm"
    if (mode == "add") { while ((getline l < entryfile) > 0) entry = entry l; close(entryfile) }
    done = 0; inlist = 0; buf = ""
}
buf != "" { buf = buf "\n" $0; if ($0 ~ /\/>/) flush(); next }
/<PLUGIN( |$)/ { buf = $0; if ($0 ~ /\/>/) flush(); next }
islist() { inlist = 1; print; next }
inlist && /<KNOWNPLUGINS\/>/ {
    if (mode == "add") {
        ind = $0; sub(/<.*/, "", ind)
        print ind "<KNOWNPLUGINS>"; print ind "  " entry; print ind "</KNOWNPLUGINS>"; done = 1
    } else print
    inlist = 0; next
}
inlist && /<\/KNOWNPLUGINS>/ {
    if (mode == "add" && !done) { ind = $0; sub(/<.*/, "", ind); print ind "  " entry; done = 1 }
    inlist = 0; print; next
}
/<\/PROPERTIES>/ {
    if (mode == "add" && !done) {
        print "  <VALUE name=\"" list "\">"; print "    <KNOWNPLUGINS>"; print "      " entry
        print "    </KNOWNPLUGINS>"; print "  </VALUE>"; done = 1
    }
    print; next
}
{ print }
