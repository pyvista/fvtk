"""
Utility module to make it easier to create new keys.
"""
from fvtk.vtkCommonCore import vtkInformationDataObjectKey as DataaObjectKey
from fvtk.vtkCommonCore import vtkInformationDoubleKey as DoubleKey
from fvtk.vtkCommonCore import vtkInformationDoubleVectorKey as DoubleVectorKey
from fvtk.vtkCommonCore import vtkInformationIdTypeKey as IdTypeKey
from fvtk.vtkCommonCore import vtkInformationInformationKey as InformationKey
from fvtk.vtkCommonCore import vtkInformationInformationVectorKey as InformationVectorKey
from fvtk.vtkCommonCore import vtkInformationIntegerKey as IntegerKey
from fvtk.vtkCommonCore import vtkInformationIntegerVectorKey as IntegerVectorKey
from fvtk.vtkCommonCore import vtkInformationKeyVectorKey as KeyVectorKey
from fvtk.vtkCommonCore import vtkInformationObjectBaseKey as ObjectBaseKey
from fvtk.vtkCommonCore import vtkInformationObjectBaseVectorKey as ObjectBaseVectorKey
from fvtk.vtkCommonCore import vtkInformationRequestKey as RequestKey
from fvtk.vtkCommonCore import vtkInformationStringKey as StringKey
from fvtk.vtkCommonCore import vtkInformationStringVectorKey as StringVectorKey
from fvtk.vtkCommonCore import vtkInformationUnsignedLongKey as UnsignedLongKey
from fvtk.vtkCommonCore import vtkInformationVariantKey as VariantKey
from fvtk.vtkCommonCore import vtkInformationVariantVectorKey as VariantVectorKey
from fvtk.vtkCommonExecutionModel import vtkInformationDataObjectMetaDataKey as DataObjectMetaDataKey
from fvtk.vtkCommonExecutionModel import vtkInformationExecutivePortKey as ExecutivePortKey
from fvtk.vtkCommonExecutionModel import vtkInformationExecutivePortVectorKey as ExecutivePortVectorKey
from fvtk.vtkCommonExecutionModel import vtkInformationIntegerRequestKey as IntegerRequestKey

def MakeKey(key_type, name, location, *args):
    """Given a key type, make a new key of given name
    and location."""
    return key_type.MakeKey(name, location, *args)
