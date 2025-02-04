/*##############################################################################

    HPCC SYSTEMS software Copyright (C) 2023 HPCC Systems®.

    Licensed under the Apache License, Version 2.0 (the "License");
    you may not use this file except in compliance with the License.
    You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0

    Unless required by applicable law or agreed to in writing, software
    distributed under the License is distributed on an "AS IS" BASIS,
    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
    See the License for the specific language governing permissions and
    limitations under the License.
############################################################################## */

//xversion url='http://.:9876'
//nothor
//nohthor

import ^ as root;

serviceUrl := #IFDEFINED(root.url, 'http://.:9876');

//--- end of version configuration ---

import common.SoapTextTest;

#stored ('searchWords', 'one,and,sheep,when,richard,king');
#stored ('url', serviceUrl);

SoapTextTest.mainService();
