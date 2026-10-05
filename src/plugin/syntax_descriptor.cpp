#include "syntax_descriptor.h"

namespace trowel {

int SyntaxRegistry::registerDescriptor(const SyntaxDescriptor& desc)
{
    int idx = static_cast<int>(descriptors_.size());
    descriptors_.append(desc);
    nameIndex_.insert(desc.name, idx);
    return idx;
}

const SyntaxDescriptor* SyntaxRegistry::find(const QString& name) const
{
    auto it = nameIndex_.constFind(name);
    if (it == nameIndex_.end()) return nullptr;
    return &descriptors_[it.value()];
}

const SyntaxDescriptor* SyntaxRegistry::findByExtension(const QString& ext) const
{
    for (const auto& desc : descriptors_) {
        if (desc.extensions.contains(ext, Qt::CaseInsensitive))
            return &desc;
    }
    return nullptr;
}

int SyntaxRegistry::index(const QString& name) const
{
    return nameIndex_.value(name, -1);
}

}  // namespace trowel
