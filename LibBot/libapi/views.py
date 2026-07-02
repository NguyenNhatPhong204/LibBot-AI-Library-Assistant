from django.shortcuts import render

def robot_display(request):
    return render(request, 'robot_display.html')

def table_call_page(request, table_id):
    return render(request, 'table_call.html', {
        'table_id': table_id
    })