#include <algorithm>
#include <iostream>
#include <vector>
using namespace std;

int main() {
    int t ;
    cin>>t;
    while(t--){
        int n ;
        cin>>n;
        vector<int> a(n);
        for(int i =0;i<n;++i){
            cin>>a[i];
        }

        int ans = a[0];
        sort(a.begin()+1,a.end());
        
        int len = a.size();
        for(auto it =a.begin()+1;it!=a.end();++it){
            ans -= len*(*it);
            len--;
        }
        cout<<ans<<'\n';
    }
}
// 64 位输出请用 printf("%lld")