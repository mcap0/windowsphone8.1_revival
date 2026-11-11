# fixes to pyOneNote 0.0.x FileNode.py: ID streams never advanced (all children = same id),
# ArrayOfPropertyValues unimplemented, nested PropertySet missing arguments
import sys
p = sys.argv[1]; s = open(p).read()
s = s.replace("""            res = self.body[self.head]
        return res""", """            res = self.body[self.head]
            self.head += 1
        return res""")
s = s.replace("""                raise NotImplementedError('ArrayOfPropertyValues is not implement')
            elif type == 0x11:
                self.rgData.append(PropertySet(file))""", """                n, = struct.unpack('<I', file.read(4))
                arr = []
                if n:
                    PropertyID(file)
                    for _ in range(n):
                        arr.append(PropertySet(file, OIDs, OSIDs, ContextIDs, document))
                self.rgData.append(arr)
            elif type == 0x11:
                self.rgData.append(PropertySet(file, OIDs, OSIDs, ContextIDs, document))""")
open(p, 'w').write(s)
# keep unknown properties as prop_XXXXXXXX and show nested property sets (note tags = checkboxes)
s = open(p).read()
s = s.replace("""            propertyName = str(self.rgPrids[i])
            if propertyName != 'Unknown':""", """            propertyName = str(self.rgPrids[i])
            if propertyName == 'Unknown':
                propertyName = 'prop_%08x' % self.rgPrids[i].value
            if True:""")
s = s.replace("""                        if isinstance(self.rgData[i], list):
                            propertyVal = [str(i) for i in self.rgData[i]]""", """                        if isinstance(self.rgData[i], list):
                            propertyVal = [i.get_properties() if isinstance(i, PropertySet) else str(i) for i in self.rgData[i]]
                        elif isinstance(self.rgData[i], PropertySet):
                            propertyVal = self.rgData[i].get_properties()""")
open(p, 'w').write(s)
